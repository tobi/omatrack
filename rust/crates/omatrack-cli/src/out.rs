//! Byte-exact buffered output (paths may not be UTF-8, and printf pads by
//! bytes).

use omatrack_core::cfmt::{Arg, sprintf};
use std::io::Write;

/// A stdout or stderr sink that buffers like stdio and writes raw bytes.
pub struct Out {
    buffer: Vec<u8>,
    stderr: bool,
}

impl Out {
    pub fn stdout() -> Self {
        Self {
            buffer: Vec::with_capacity(4096),
            stderr: false,
        }
    }
    pub fn stderr() -> Self {
        Self {
            buffer: Vec::new(),
            stderr: true,
        }
    }
    pub fn str(&mut self, text: &str) -> &mut Self {
        self.buffer.extend_from_slice(text.as_bytes());
        self
    }
    pub fn bytes(&mut self, bytes: &[u8]) -> &mut Self {
        self.buffer.extend_from_slice(bytes);
        self
    }
    /// printf-style formatting through the C library.
    pub fn printf(&mut self, format: &str, args: &[Arg<'_>]) -> &mut Self {
        self.buffer
            .extend_from_slice(sprintf(format, args).as_bytes());
        self
    }
    pub fn flush(&mut self) {
        let result = if self.stderr {
            std::io::stderr().lock().write_all(&self.buffer)
        } else {
            let mut out = std::io::stdout().lock();
            out.write_all(&self.buffer).and_then(|()| out.flush())
        };
        #[expect(
            clippy::let_underscore_must_use,
            reason = "Preserve the frozen CLI stdio behavior: output failure, including a closed pipe, does not change its exit status."
        )]
        let _ = result;
        self.buffer.clear();
    }
}

impl Drop for Out {
    fn drop(&mut self) {
        if !self.buffer.is_empty() {
            self.flush();
        }
    }
}

/// `printf!(out, "fmt", args...)`.
#[macro_export]
macro_rules! printf {
    ($out:expr, $format:expr $(, $arg:expr)* $(,)?) => {
        $out.printf($format, &[$(omatrack_core::cfmt::Arg::from($arg)),*])
    };
}
