#pragma once

// Stable C ABI for embedding the C++ VPN core in a native front-end.
//
// The ABI intentionally exposes no C++ types, STL containers, exceptions or
// Json::Value objects.  JSON is used only as an in-process wire format at the
// language boundary; it never goes through a TCP socket in embedded mode.

#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define PPP_CORE_API __attribute__((visibility("default")))
#else
#define PPP_CORE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ppp_core_handle ppp_core_handle;

typedef void (*ppp_core_log_callback)(
    void* user_data,
    const char* level,
    const char* line);

// Starts one core runtime on its own internal C++ executor thread.  argv uses
// the normal argc/argv convention, including argv[0] as the program name;
// argv is copied before this function returns and may be released by the
// caller. Returns NULL on startup failure. error_buffer is optional and UTF-8.
PPP_CORE_API ppp_core_handle* ppp_core_start(
    int argc,
    const char* const* argv,
    ppp_core_log_callback log_callback,
    void* user_data,
    char* error_buffer,
    size_t error_buffer_size);

// Execute a control command on the core executor.  params_json may be NULL
// and is treated as {}.  The returned JSON must be released with
// ppp_core_free_string().  Returns non-zero on success.
PPP_CORE_API int ppp_core_command(
    ppp_core_handle* handle,
    const char* method,
    const char* params_json,
    char** result_json,
    char* error_buffer,
    size_t error_buffer_size);

// Convenience aliases for the most frequent calls.
PPP_CORE_API int ppp_core_snapshot(
    ppp_core_handle* handle,
    char** snapshot_json,
    char* error_buffer,
    size_t error_buffer_size);

PPP_CORE_API int ppp_core_set_log_level(
    ppp_core_handle* handle,
    const char* level,
    char* error_buffer,
    size_t error_buffer_size);

// Returns non-zero while the core executor is alive.  This is used by a
// front-end to detect an unexpected in-process shutdown without polling a
// child-process handle.
PPP_CORE_API int ppp_core_is_running(ppp_core_handle* handle);

// Requests a synchronous network cleanup and waits for the core executor to
// finish.  The call is idempotent.  Returns non-zero when cleanup completed.
PPP_CORE_API int ppp_core_stop(
    ppp_core_handle* handle,
    char* error_buffer,
    size_t error_buffer_size);

// Stops if necessary, joins the executor thread and releases the opaque
// handle.  The pointer must not be used afterwards.
PPP_CORE_API void ppp_core_destroy(ppp_core_handle* handle);

PPP_CORE_API void ppp_core_free_string(char* value);

#ifdef __cplusplus
}
#endif

#undef PPP_CORE_API
