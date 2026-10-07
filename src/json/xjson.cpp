#include "json/xjson.h"

#include <fstream>

#include "cJSON.h"
#include "file/xfile.h"
#include "log/xerror.h"
#include "log/xlogger.h"

namespace au {
namespace json {

// ============================================================================
// XJsonValue
// ============================================================================

XJsonValue::XJsonValue(cJSON* p) : mJson(p) {}
XJsonValue::~XJsonValue() = default;

bool XJsonValue::isValid() const { return mJson != nullptr && !cJSON_IsInvalid(mJson); }
bool XJsonValue::isNumber() const { return cJSON_IsNumber(mJson); }
bool XJsonValue::isBool() const { return cJSON_IsBool(mJson); }
bool XJsonValue::isString() const { return cJSON_IsString(mJson); }
bool XJsonValue::isArray() const { return cJSON_IsArray(mJson); }
bool XJsonValue::isObject() const { return cJSON_IsObject(mJson); }

std::string XJsonValue::getType() const
{
    XCHECK_WITH_RET(isValid(), "JSON_NULL");
    switch (mJson->type) {
        case cJSON_False:
        case cJSON_True: return "JSON_BOOL";
        case cJSON_Number: return "JSON_NUMBER";
        case cJSON_String: return "JSON_STRING";
        case cJSON_Array: return "JSON_ARRAY";
        case cJSON_Object: return "JSON_OBJECT";
    }
    return "JSON_UNKNOWN";
}

int XJsonValue::getInt() const
{
    XCHECK_WITH_RET(isValid() && isNumber(), 0);
    return mJson->valueint;
}

float XJsonValue::getFloat() const
{
    XCHECK_WITH_RET(isValid() && isNumber(), 0.0f);
    return static_cast<float>(mJson->valuedouble);
}

double XJsonValue::getDouble() const
{
    XCHECK_WITH_RET(isValid() && isNumber(), 0.0);
    return mJson->valuedouble;
}

bool XJsonValue::getBool() const
{
    XCHECK_WITH_RET(isValid() && isBool(), false);
    return cJSON_IsTrue(mJson);
}

std::string XJsonValue::getString() const
{
    XCHECK_WITH_RET(isValid() && isString(), std::string());
    return std::string(mJson->valuestring);
}

size_t XJsonValue::getArraySize() const
{
    XCHECK_WITH_RET(isValid() && isArray(), 0);
    return static_cast<size_t>(cJSON_GetArraySize(mJson));
}

XJsonValue XJsonValue::operator[](size_t idx) const
{
    XCHECK_WITH_RET(isValid() && isArray() && idx < getArraySize(), XJsonValue(nullptr));
    return XJsonValue(cJSON_GetArrayItem(mJson, static_cast<int>(idx)));
}

XJsonValue XJsonValue::operator[](const std::string& key) const
{
    XCHECK_WITH_RET(isValid() && isObject(), XJsonValue(nullptr));
    return XJsonValue(cJSON_GetObjectItemCaseSensitive(mJson, key.c_str()));
}

bool XJsonValue::hasKey(const std::string& key) const
{
    if (!isValid() || !isObject())
        return false;
    return cJSON_GetObjectItemCaseSensitive(mJson, key.c_str()) != nullptr;
}

// ============================================================================
// XJson
// ============================================================================

XJson::XJson() = default;

XJson::XJson(const std::string& filename) { parseFile(filename); }

XJson::~XJson() { clear(); }

XJson::XJson(XJson&& other) noexcept : mRoot(other.mRoot), mOwned(other.mOwned)
{
    other.mRoot  = nullptr;
    other.mOwned = false;
}

XJson& XJson::operator=(XJson&& other) noexcept
{
    if (this != &other) {
        clear();
        mRoot        = other.mRoot;
        mOwned       = other.mOwned;
        other.mRoot  = nullptr;
        other.mOwned = false;
    }
    return *this;
}

XJson XJson::parse(const std::string& jsonString)
{
    XJson j;
    j.parseString(jsonString);
    return j;
}

XJson XJson::object()
{
    XJson j;
    j.mRoot  = cJSON_CreateObject();
    j.mOwned = true;
    return j;
}

XJson XJson::array()
{
    XJson j;
    j.mRoot  = cJSON_CreateArray();
    j.mOwned = true;
    return j;
}

int XJson::parseFile(const std::string& filename)
{
    std::string content;
    int         ret = au::file::read(filename, content);
    XCHECK_WITH_RET(ret == err::kSuccess, ret);
    return parseString(content);
}

int XJson::parseString(const std::string& str)
{
    mRoot = cJSON_Parse(str.c_str());
    XCHECK_WITH_MSG(mRoot != nullptr, err::kErrorBadFormat, "JSON parse error before: '%s'", cJSON_GetErrorPtr());
    mOwned = true;
    return err::kSuccess;
}

void XJson::clear()
{
    if (mRoot && mOwned) {
        cJSON_Delete(mRoot);
    }
    mRoot  = nullptr;
    mOwned = false;
}

bool XJson::isValid() const { return mRoot != nullptr && !cJSON_IsInvalid(mRoot); }

XJsonValue XJson::operator[](const std::string& key) const
{
    XCHECK_WITH_RET(isValid(), XJsonValue(nullptr));
    return XJsonValue(cJSON_GetObjectItemCaseSensitive(mRoot, key.c_str()));
}

XJson& XJson::set(const std::string& key, int value)
{
    if (cJSON_GetObjectItemCaseSensitive(mRoot, key.c_str()))
        cJSON_ReplaceItemInObjectCaseSensitive(mRoot, key.c_str(), cJSON_CreateNumber(value));
    else
        cJSON_AddItemToObject(mRoot, key.c_str(), cJSON_CreateNumber(value));
    return *this;
}

XJson& XJson::set(const std::string& key, double value)
{
    if (cJSON_GetObjectItemCaseSensitive(mRoot, key.c_str()))
        cJSON_ReplaceItemInObjectCaseSensitive(mRoot, key.c_str(), cJSON_CreateNumber(value));
    else
        cJSON_AddItemToObject(mRoot, key.c_str(), cJSON_CreateNumber(value));
    return *this;
}

XJson& XJson::set(const std::string& key, bool value)
{
    if (cJSON_GetObjectItemCaseSensitive(mRoot, key.c_str()))
        cJSON_ReplaceItemInObjectCaseSensitive(mRoot, key.c_str(), cJSON_CreateBool(value));
    else
        cJSON_AddItemToObject(mRoot, key.c_str(), cJSON_CreateBool(value));
    return *this;
}

XJson& XJson::set(const std::string& key, const std::string& value) { return set(key, value.c_str()); }

XJson& XJson::set(const std::string& key, const char* value)
{
    if (cJSON_GetObjectItemCaseSensitive(mRoot, key.c_str()))
        cJSON_ReplaceItemInObjectCaseSensitive(mRoot, key.c_str(), cJSON_CreateString(value));
    else
        cJSON_AddItemToObject(mRoot, key.c_str(), cJSON_CreateString(value));
    return *this;
}

bool XJson::remove(const std::string& key)
{
    if (!isValid())
        return false;
    cJSON_DeleteItemFromObjectCaseSensitive(mRoot, key.c_str());
    return true;
}

XJson& XJson::append(int value)
{
    cJSON_AddItemToArray(mRoot, cJSON_CreateNumber(value));
    return *this;
}

XJson& XJson::append(double value)
{
    cJSON_AddItemToArray(mRoot, cJSON_CreateNumber(value));
    return *this;
}

XJson& XJson::append(const std::string& value)
{
    cJSON_AddItemToArray(mRoot, cJSON_CreateString(value.c_str()));
    return *this;
}

bool XJson::hasKey(const std::string& key) const
{
    if (!isValid())
        return false;
    return cJSON_GetObjectItemCaseSensitive(mRoot, key.c_str()) != nullptr;
}

std::vector<std::string> XJson::keys() const
{
    std::vector<std::string> result;
    if (!isValid() || !cJSON_IsObject(mRoot))
        return result;
    cJSON* item = mRoot->child;
    while (item) {
        if (item->string)
            result.emplace_back(item->string);
        item = item->next;
    }
    return result;
}

std::string XJson::dump(int indent) const
{
    if (!isValid())
        return "{}";
    char*       str = indent > 0 ? cJSON_Print(mRoot) : cJSON_PrintUnformatted(mRoot);
    std::string result(str);
    cJSON_free(str);
    return result;
}

int XJson::save(const std::string& filename, int indent) const
{
    std::string   content = dump(indent);
    std::ofstream ofs(filename);
    XCHECK_WITH_RET(ofs.is_open(), err::kErrorOpenFailed);
    ofs << content;
    return err::kSuccess;
}

}  // namespace json
}  // namespace au
