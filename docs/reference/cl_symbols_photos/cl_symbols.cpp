#include "cl_symbols.h"

// AURA_USING_AU
using namespace au;

// Shorthand for resolving and calling a dynamically loaded CL symbol.
// Each function below is a trampoline: it resolves the real OpenCL symbol
// from the dynamically loaded library and forwards the call.
#define CL_FWD(name) XDLIB_GET(gpu::CLSymbols::lib(), name)

// ============================================================================
// platform
// ============================================================================

cl_int CL_API_CALL clGetPlatformIDs(cl_uint num_entries, cl_platform_id* platforms, cl_uint* num_platforms)
{
    return CL_FWD(clGetPlatformIDs)(num_entries, platforms, num_platforms);
}

cl_int CL_API_CALL clGetPlatformInfo(cl_platform_id   platform,
                                   cl_platform_info param_name,
                                   size_t           param_value_size,
                                   void*            param_value,
                                   size_t*          param_value_size_ret)
{
    return CL_FWD(clGetPlatformInfo)(platform, param_name, param_value_size, param_value, param_value_size_ret);
}

// ============================================================================
// device
// ============================================================================

cl_int CL_API_CALL clRetainDevice(cl_device_id device)
{
    return CL_FWD(clRetainDevice)(device);
}

cl_int CL_API_CALL clReleaseDevice(cl_device_id device)
{
    return CL_FWD(clReleaseDevice)(device);
}

cl_int CL_API_CALL clGetDeviceIDs(cl_platform_id platform,
                                cl_device_type device_type,
                                cl_uint        num_entries,
                                cl_device_id*  devices,
                                cl_uint*       num_devices)
{
    return CL_FWD(clGetDeviceIDs)(platform, device_type, num_entries, devices, num_devices);
}

cl_int CL_API_CALL clGetDeviceInfo(cl_device_id   device,
                                 cl_device_info param_name,
                                 size_t         param_value_size,
                                 void*          param_value,
                                 size_t*        param_value_size_ret)
{
    return CL_FWD(clGetDeviceInfo)(device, param_name, param_value_size, param_value, param_value_size_ret);
}

// ============================================================================
// context
// ============================================================================

cl_int CL_API_CALL clRetainContext(cl_context context)
{
    return CL_FWD(clRetainContext)(context);
}

cl_int CL_API_CALL clReleaseContext(cl_context context)
{
    return CL_FWD(clReleaseContext)(context);
}

cl_context CL_API_CALL clCreateContextFromType(
    const cl_context_properties* properties,
    cl_device_type               device_type,
    void(CL_CALLBACK* pfn_notify)(const char* errinfo, const void* private_info, size_t cb, void* user_data),
    void*                        user_data,
    cl_int*                      errcode_ret)
{
    return CL_FWD(clCreateContextFromType)(properties, device_type, pfn_notify, user_data, errcode_ret);
}

cl_int CL_API_CALL clGetContextInfo(cl_context      context,
                                  cl_context_info param_name,
                                  size_t          param_value_size,
                                  void*           param_value,
                                  size_t*         param_value_size_ret)
{
    return CL_FWD(clGetContextInfo)(context, param_name, param_value_size, param_value, param_value_size_ret);
}

// PHOTO GAP: original lines 94–431 are not fully provided.
// The visible beginning of clCreateContext (lines 94–97) is:
// cl_context CL_API_CALL clCreateContext(
//     const cl_context_properties* properties,
//     cl_uint num_devices,
//     const cl_device_id* devices,
// Line 98 is clipped. Its remainder and the body are not transcribed.
// Only the clRetainEvent signature at original line 427 is visible:
// cl_int CL_API_CALL clRetainEvent(cl_event event)
// Its body is not visible. No implementation is invented here.

// PHOTO RESUMES: original lines 432–476, cl-symbols1.jpg.
cl_int CL_API_CALL clGetEventProfilingInfo(cl_event          event,
                                         cl_profiling_info param_name,
                                         size_t            param_value_size,
                                         void*             param_value,
                                         size_t*           param_value_size_ret)
{
    return CL_FWD(clGetEventProfilingInfo)(event, param_name, param_value_size, param_value, param_value_size_ret);
}

cl_int CL_API_CALL clSetEventCallback(cl_event event,
                                    cl_int   command_exec_callback_type,
                                    void(CL_CALLBACK* pfn_notify)(cl_event, cl_int, void*),
                                    void*    user_data)
{
    return CL_FWD(clSetEventCallback)(event, command_exec_callback_type, pfn_notify, user_data);
}

// ============================================================================
// queue sync
// ============================================================================

cl_int CL_API_CALL clFlush(cl_command_queue command_queue)
{
    return CL_FWD(clFlush)(command_queue);
}

cl_int CL_API_CALL clFinish(cl_command_queue command_queue)
{
    return CL_FWD(clFinish)(command_queue);
}

// ============================================================================
// extension function resolution
// ============================================================================

// OpenCL §9.2 spec-mandated path for resolving non-ICD-dispatch vendor
// extensions (e.g. clImportMemoryARM, clGetDeviceImageInfoQCOM). Direct dlsym
// of those symbols fails on ROMs lacking a Shim layer; this standard API is
// the correct resolution route and is provided by every OpenCL 1.2+ runtime.
void* CL_API_CALL clGetExtensionFunctionAddressForPlatform(cl_platform_id platform, const char* func_name)
{
    return CL_FWD(clGetExtensionFunctionAddressForPlatform)(platform, func_name);
}

#undef CL_FWD
