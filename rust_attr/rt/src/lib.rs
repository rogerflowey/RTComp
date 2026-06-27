//! Facade crate re-exporting the `rt_attr` procedural macros under the
//! `#[rt::*]` namespace. Add to your `Cargo.toml`:
//!
//! ```toml
//! rt = { path = "<path-to-rust_attr>/rt" }
//! ```
//!
//! Then import the attribute macros and apply them to your real-time
//! entry points:
//!
//! ```rust,ignore
//! use rt::nonblocking;
//!
//! #[rt::nonblocking]
//! #[rt::nonallocating]
//! extern "C" fn audio_callback() { ... }
//! ```

pub use rt_attr::{
    nonblocking, nonallocating, nolock, nothrow, norecurse, async_signal_safe,
    stack_bound,
};