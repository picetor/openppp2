//! Rust FFI wrapper for the statically linked C++ core.
//!
//! This module is compiled only when build.rs finds `ppp-core.lib`/`libppp-core`.
//! The wrapper owns the C++ handle and keeps all blocking calls off the UI
//! thread; the higher-level client in `rpc::mod` turns completed calls into the
//! existing typed Response model.

#[cfg(ppp_in_process_core)]
mod enabled {
    use anyhow::{bail, Context, Result};
    use std::ffi::{CStr, CString};
    use std::os::raw::{c_char, c_int, c_void};
    use std::ptr::NonNull;
    use std::sync::{Arc, Mutex, OnceLock};

    #[repr(C)]
    struct RawCoreHandle {
        _private: [u8; 0],
    }

    extern "C" {
        fn ppp_core_start(
            argc: c_int,
            argv: *const *const c_char,
            log_callback: Option<extern "C" fn(*mut c_void, *const c_char, *const c_char)>,
            user_data: *mut c_void,
            error_buffer: *mut c_char,
            error_buffer_size: usize,
        ) -> *mut RawCoreHandle;
        fn ppp_core_command(
            handle: *mut RawCoreHandle,
            method: *const c_char,
            params_json: *const c_char,
            result_json: *mut *mut c_char,
            error_buffer: *mut c_char,
            error_buffer_size: usize,
        ) -> c_int;
        fn ppp_core_set_log_level(
            handle: *mut RawCoreHandle,
            level: *const c_char,
            error_buffer: *mut c_char,
            error_buffer_size: usize,
        ) -> c_int;
        fn ppp_core_is_running(handle: *mut RawCoreHandle) -> c_int;
        fn ppp_core_stop(
            handle: *mut RawCoreHandle,
            error_buffer: *mut c_char,
            error_buffer_size: usize,
        ) -> c_int;
        fn ppp_core_destroy(handle: *mut RawCoreHandle);
        fn ppp_core_free_string(value: *mut c_char);
    }

    const ERROR_BUFFER_SIZE: usize = 2048;

    // The Windows console-close callback runs outside the terminal event
    // loop. Keep only a shared owner handle here; the actual C++ API still
    // serializes stop/command calls and performs synchronous network cleanup.
    static EMERGENCY_CORE: OnceLock<Mutex<Option<Arc<Mutex<CoreHandle>>>>> = OnceLock::new();

    fn emergency_slot() -> &'static Mutex<Option<Arc<Mutex<CoreHandle>>>> {
        EMERGENCY_CORE.get_or_init(|| Mutex::new(None))
    }

    fn read_error(buffer: &[u8]) -> String {
        let end = buffer
            .iter()
            .position(|value| *value == 0)
            .unwrap_or(buffer.len());
        String::from_utf8_lossy(&buffer[..end]).into_owned()
    }

    pub struct CoreHandle {
        raw: NonNull<RawCoreHandle>,
    }

    // The C++ API serializes commands internally and the opaque handle never
    // exposes C++ memory to Rust. It is therefore safe to move/share the
    // wrapper between the command worker and the UI owner.
    unsafe impl Send for CoreHandle {}
    unsafe impl Sync for CoreHandle {}

    impl CoreHandle {
        pub fn start(args: &[String]) -> Result<Self> {
            // The C++ command-line parser follows the normal argc/argv
            // convention and deliberately starts scanning at argv[1].  The
            // Rust launcher stores only the extra core arguments, so without
            // a synthetic argv[0] the first option (commonly --lwip=yes) is
            // silently ignored and the core falls back to CTCP.
            let mut arguments = Vec::with_capacity(args.len() + 1);
            arguments.push(CString::new("ppp-tui-core").context("core argv[0] contains NUL")?);
            arguments.extend(
                args.iter()
                    .map(|value| CString::new(value.as_str()).context("core argument contains NUL"))
                    .collect::<Result<Vec<_>>>()?,
            );
            let pointers = arguments
                .iter()
                .map(|value| value.as_ptr())
                .collect::<Vec<_>>();
            let mut error = vec![0u8; ERROR_BUFFER_SIZE];
            let raw = unsafe {
                ppp_core_start(
                    pointers.len() as c_int,
                    if pointers.is_empty() {
                        std::ptr::null()
                    } else {
                        pointers.as_ptr()
                    },
                    None,
                    std::ptr::null_mut(),
                    error.as_mut_ptr().cast(),
                    error.len(),
                )
            };
            let raw = NonNull::new(raw).ok_or_else(|| {
                let message = read_error(&error);
                let message = if message.is_empty() {
                    "core startup failed".to_string()
                } else {
                    message
                };
                anyhow::anyhow!(message)
            })?;
            Ok(Self { raw })
        }

        pub fn command(&self, method: &str, params_json: &str) -> Result<serde_json::Value> {
            let method = CString::new(method).context("core method contains NUL")?;
            let params = CString::new(params_json).context("core parameters contain NUL")?;
            let mut result: *mut c_char = std::ptr::null_mut();
            let mut error = vec![0u8; ERROR_BUFFER_SIZE];
            let ok = unsafe {
                ppp_core_command(
                    self.raw.as_ptr(),
                    method.as_ptr(),
                    params.as_ptr(),
                    &mut result,
                    error.as_mut_ptr().cast(),
                    error.len(),
                )
            } != 0;
            if !ok {
                bail!("{}", read_error(&error));
            }
            if result.is_null() {
                return Ok(serde_json::Value::Object(serde_json::Map::new()));
            }
            let value = unsafe { CStr::from_ptr(result) }
                .to_str()
                .context("core returned non-UTF-8 JSON")
                .and_then(|text| serde_json::from_str(text).context("invalid core result JSON"));
            unsafe { ppp_core_free_string(result) };
            Ok(value?)
        }

        pub fn set_log_level(&self, level: &str) -> Result<()> {
            let level = CString::new(level).context("log level contains NUL")?;
            let mut error = vec![0u8; ERROR_BUFFER_SIZE];
            let ok = unsafe {
                ppp_core_set_log_level(
                    self.raw.as_ptr(),
                    level.as_ptr(),
                    error.as_mut_ptr().cast(),
                    error.len(),
                )
            } != 0;
            if ok {
                Ok(())
            } else {
                bail!("{}", read_error(&error));
            }
        }

        pub fn is_running(&self) -> bool {
            unsafe { ppp_core_is_running(self.raw.as_ptr()) != 0 }
        }

        pub fn stop(&self) -> Result<()> {
            let mut error = vec![0u8; ERROR_BUFFER_SIZE];
            let ok =
                unsafe { ppp_core_stop(self.raw.as_ptr(), error.as_mut_ptr().cast(), error.len()) }
                    != 0;
            if ok {
                Ok(())
            } else {
                bail!("{}", read_error(&error));
            }
        }
    }

    pub fn register_emergency_core(core: Arc<Mutex<CoreHandle>>) {
        *emergency_slot()
            .lock()
            .unwrap_or_else(|poison| poison.into_inner()) = Some(core);
    }

    pub fn clear_emergency_core() {
        *emergency_slot()
            .lock()
            .unwrap_or_else(|poison| poison.into_inner()) = None;
    }

    /// Stop the currently registered in-process core from an OS console
    /// callback. This is intentionally synchronous so the callback does not
    /// return while DNS/routes/TUN state is still owned by the core.
    pub fn emergency_stop() -> bool {
        let core = emergency_slot()
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .clone();
        let Some(core) = core else {
            return false;
        };
        core.lock().map(|core| core.stop().is_ok()).unwrap_or(false)
    }

    pub fn emergency_core_registered() -> bool {
        emergency_slot()
            .lock()
            .map(|core| core.is_some())
            .unwrap_or(false)
    }

    impl Drop for CoreHandle {
        fn drop(&mut self) {
            unsafe { ppp_core_destroy(self.raw.as_ptr()) };
        }
    }
}

#[cfg(ppp_in_process_core)]
pub use enabled::{
    clear_emergency_core, emergency_core_registered, emergency_stop, register_emergency_core,
    CoreHandle,
};
