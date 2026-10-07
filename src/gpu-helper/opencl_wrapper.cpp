#include "opencl_wrapper.h"

#include <functional>
#include <utility>

#include "vivo_comdef.h"
#include "file/xfile.h"
#include "file/xpath.h"
#include "log/xlogger.h"
#include "sys/xsystem.h"

namespace au {
namespace gpu {

// ---------------------------------------------------------------------------
// Pending-handle bag: kept alive until the kernel completes, then released on
// the driver callback thread. Releasing only calls clReleaseMemObject (via the
// cl::Buffer destructor), which does not re-enter OpenCL enqueue paths, so it
// is safe on the callback thread.
// ---------------------------------------------------------------------------
namespace {

struct PendingBag
{
    std::vector<std::shared_ptr<OpenCLBuffer>> handles;
};

void CL_CALLBACK onKernelCompleted(cl_event /*event*/, cl_int /*status*/, void* user)
{
    delete static_cast<PendingBag*>(user);
}

}  // namespace

OpenCLWrapper& OpenCLWrapper::get()
{
    static OpenCLWrapper instance;
    return instance;
}

OpenCLWrapper::OpenCLWrapper() = default;

OpenCLWrapper::~OpenCLWrapper()
{
    XCHECK(!mCommandQueue() && "OpenCLWrapper need to deinit!");
}

bool OpenCLWrapper::available() const noexcept
{
    return !mPlatforms.empty() && !mDevices.empty() && mContext() && mCommandQueue();
}

int OpenCLWrapper::init(std::string folderBinary, bool enableProfiling)
{
    if (mCommandQueue()) {
        deinit();
    }

    mFolderBinary = file::XPath(folderBinary + "/").parent().string();
    XCHECK_WITH_RET(!mFolderBinary.empty(), VDKResultEInvalidParam);
    XCHECK_WITH_RET(file::existDir(mFolderBinary), VDKResultEInvalidParam);
    XCHECK_WITH_RET(sys::isMediaTekPlatform() ^ sys::isQualcommPlatform(), VDKResultEBadState);

    int retPlatform = getPlatform();
    XCHECK_WITH_MSG(retPlatform == CL_SUCCESS, retPlatform, "clError: %s\n", clErrorInfo(retPlatform).c_str());

    int retDevice = getDevice();
    XCHECK_WITH_MSG(retDevice == CL_SUCCESS, retDevice, "clError: %s\n", clErrorInfo(retDevice).c_str());

    int retContext = createContext();
    XCHECK_WITH_MSG(retContext == CL_SUCCESS, retContext, "clError: %s\n", clErrorInfo(retContext).c_str());

    mEnableProfiling = enableProfiling;
    int retQueue = createCommandQueue(mEnableProfiling ? CL_QUEUE_PROFILING_ENABLE : 0);
    XCHECK_WITH_MSG(retQueue == CL_SUCCESS, retQueue, "clError: %s\n", clErrorInfo(retQueue).c_str());

    return VDKResultSuccess;
}

int OpenCLWrapper::deinit()
{
    mLocal  = ::cl::NDRange();
    mGlobal = ::cl::NDRange();
    mOffset = ::cl::NDRange();

    mKernels.clear();
    mCommandQueue = ::cl::CommandQueue();
    mContext      = ::cl::Context();
    mDevices.clear();
    mPlatforms.clear();

    return VDKResultSuccess;
}

OpenCLWrapper& OpenCLWrapper::setNDRange(::cl::NDRange global, ::cl::NDRange local, ::cl::NDRange offset)
{
    mGlobal = global;
    mLocal  = local;
    mOffset = offset;
    return *this;
}

int OpenCLWrapper::flush()
{
    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);
    int ret = mCommandQueue.flush();
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s\n", clErrorInfo(ret).c_str());
    return VDKResultSuccess;
}

int OpenCLWrapper::finish()
{
    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);
    int ret = mCommandQueue.finish();
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s\n", clErrorInfo(ret).c_str());
    return VDKResultSuccess;
}

void OpenCLWrapper::attachPending(::cl::Event& evt, std::vector<std::shared_ptr<OpenCLBuffer>>&& pending)
{
    if (pending.empty() || !evt()) {
        return;
    }
    auto* bag = new PendingBag{std::move(pending)};
    cl_int rc = clSetEventCallback(evt(), CL_COMPLETE, &onKernelCompleted, bag);
    if (rc != CL_SUCCESS) {
        // Driver refused the callback: release synchronously to avoid a leak.
        // A finish() before teardown still guarantees the kernel has completed.
        XLOG_E("clSetEventCallback failed: %s, releasing handles synchronously\n", clErrorInfo(rc).c_str());
        delete bag;
    }
}

// ----- infrastructure -------------------------------------------------------

int OpenCLWrapper::getPlatform()
{
    return ::cl::Platform::get(&mPlatforms);
}

int OpenCLWrapper::getDevice()
{
    XCHECK_WITH_RET(!mPlatforms.empty(), VDKResultEInvalidParam);
    return mPlatforms.front().getDevices(CL_DEVICE_TYPE_GPU, &mDevices);
}

int OpenCLWrapper::createContext()
{
    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);
    int ret = CL_SUCCESS;
    mContext = ::cl::Context(mDevices, nullptr, nullptr, nullptr, &ret);
    return ret;
}

int OpenCLWrapper::createCommandQueue(cl_command_queue_properties properties)
{
    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);
    int ret = CL_SUCCESS;
    mCommandQueue = ::cl::CommandQueue(mContext, mDevices.front(), properties, &ret);
    return ret;
}

int OpenCLWrapper::deviceKey(std::string& key)
{
    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);

    int ret = CL_SUCCESS;
    std::string name    = mDevices.front().getInfo<CL_DEVICE_NAME>(&ret);
    std::string vendor  = mDevices.front().getInfo<CL_DEVICE_VENDOR>(&ret);
    std::string version = mDevices.front().getInfo<CL_DEVICE_VERSION>(&ret);
    std::string driver  = mDevices.front().getInfo<CL_DRIVER_VERSION>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s\n", clErrorInfo(ret).c_str());

    key = std::to_string(std::hash<std::string>{}(name + vendor + version + driver));
    key.resize(20, 'X');
    return VDKResultSuccess;
}

int OpenCLWrapper::requireKernel(::cl::Kernel& kernel, const std::string& nameKernel, const char* kernelSource,
                                 const std::string& optionsCompile)
{
    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);

    // 1. kernel cache
    auto itKernel = mKernels.find(nameKernel);
    if (itKernel != mKernels.end()) {
        kernel = itKernel->second;
        return VDKResultSuccess;
    }

    std::string keyDevice;
    XCHECK_WITH_RET(deviceKey(keyDevice) == VDKResultSuccess, VDKResultEBadState);

    std::string source = "#line 1 \"" + nameKernel + ".cl\"\n";
    source += kernelSource ? kernelSource : "";

    std::string keySource = std::to_string(std::hash<std::string>{}(source + optionsCompile));
    keySource.resize(20, 'X');

    std::string pathBin = mFolderBinary + "/" + nameKernel + "_" + keyDevice + "_" + keySource + ".bin";

    ::cl::Program program;

    // 2. cached binary
    if (file::exists(pathBin)) {
        std::string binStr;
        if (file::read(pathBin, binStr) == VDKResultSuccess && !binStr.empty()) {
            std::vector<unsigned char> bin(binStr.cbegin(), binStr.cend());
            ::cl::Program::Binaries bins = {bin};

            int ret = CL_SUCCESS;
            program = ::cl::Program(mContext, mDevices, bins, nullptr, &ret);
            if (ret == CL_SUCCESS) {
                int retBuild = program.build(mDevices, optionsCompile.c_str());
                if (retBuild != CL_SUCCESS) {
                    XLOG_E("cl build (binary) log [%s]: %s\n", nameKernel.c_str(),
                           program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(mDevices.front()).c_str());
                    program = ::cl::Program();  // fall through to JIT
                }
            }
        }
    }

    // 3. JIT from source; persist the binary for next time
    if (program.get() == nullptr) {
        XLOG_I("OpenCLWrapper::requireKernel JIT compiling kernel[%s]\n", nameKernel.c_str());

        int ret = CL_SUCCESS;
        program = ::cl::Program(mContext, source, false, &ret);
        XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s\n", clErrorInfo(ret).c_str());

        int retBuild = program.build(mDevices, optionsCompile.c_str());
        if (retBuild != CL_SUCCESS) {
            XLOG_E("cl build log [%s]: %s\n", nameKernel.c_str(),
                   program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(mDevices.front()).c_str());
            return retBuild;
        }

        ::cl::Program::Binaries bins = program.getInfo<CL_PROGRAM_BINARIES>(&ret);
        if (ret == CL_SUCCESS && !bins.empty()) {
            file::write(std::string(bins.front().cbegin(), bins.front().cend()), pathBin);
        }
    }

    // 4. create & cache
    int retCreate = CL_SUCCESS;
    kernel = ::cl::Kernel(program, nameKernel.c_str(), &retCreate);
    XCHECK_WITH_MSG(retCreate == CL_SUCCESS, retCreate, "clError: %s\n", clErrorInfo(retCreate).c_str());

    mKernels[nameKernel] = kernel;
    return VDKResultSuccess;
}

}  // namespace gpu
}  // namespace au
