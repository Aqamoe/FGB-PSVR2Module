# 从 lock 文件（{version,url,sha256}）拉取依赖。
#
# 模块仓库不复制 SDK 头文件：接口版本由 sdk.lock.json 唯一决定，
# SDK 发新版时由 sync-sdk 机器人改 lock，而不是人工拷贝。
if(FGB_DEPENDENCY_CMAKE_INCLUDED)
    return()
endif()
set(FGB_DEPENDENCY_CMAKE_INCLUDED TRUE)

include(FetchContent)

function(fgb_fetch_from_lock dep_name lock_path)
    if(NOT EXISTS "${lock_path}")
        message(FATAL_ERROR "缺少依赖锁文件：${lock_path}")
    endif()

    file(READ "${lock_path}" _lock_json)
    string(JSON _ver GET "${_lock_json}" version)
    string(JSON _url GET "${_lock_json}" url)
    string(JSON _sha GET "${_lock_json}" sha256)

    if(_url STREQUAL "" OR _sha STREQUAL "")
        message(FATAL_ERROR "锁文件字段不完整：${lock_path}")
    endif()

    set(${dep_name}_VERSION "${_ver}" PARENT_SCOPE)

    FetchContent_Declare(${dep_name}
        URL "${_url}"
        URL_HASH SHA256=${_sha}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(${dep_name})
endfunction()

# 读模块清单里的字符串字段（发布流程用它核对 tag 与 Version 一致）
function(fgb_read_manifest_string manifest_path key out_var)
    file(READ "${manifest_path}" _manifest_json)
    string(JSON _value GET "${_manifest_json}" ${key})
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()
