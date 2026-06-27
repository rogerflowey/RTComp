//! Real-time annotation attribute macros for Rust.
//!
//! Each `#[rt::<constraint>]` attribute emits a `#[used] static` marker
//! global whose link name encodes the annotated function name and constraint.
//! The RTEffect LLVM pass plugin scans the module for globals matching the
//! pattern `__rt_annot_<fn>_<constraint>` and lifts them into LLVM function
//! attributes, so `rt-constraint-check` can verify them exactly like the
//! C++ `[[rt::*]]` Clang-attribute pathway.
//!
//! The macro autonomously pushes `#[no_mangle]` onto the function so that its
//! IR-level symbol name equals the Rust fn name — this is required for the
//! marker to resolve back to the matching `Function`. Private/non-`pub`
//! functions are promoted to `pub` for the same reason.
//!
//! Supported attributes:
//!   - `#[rt::nonblocking]`
//!   - `#[rt::nonallocating]`
//!   - `#[rt::nolock]`
//!   - `#[rt::nothrow]`
//!   - `#[rt::norecurse]`
//!   - `#[rt::async_signal_safe]`
//!   - `#[rt::stack_bound(N)]` where N is an integer literal

use proc_macro::TokenStream;
use quote::{format_ident, quote};
use syn::{parse_macro_input, parse_quote, ItemFn, Visibility};

fn expand(item: TokenStream, constraint: &str, arg: Option<String>) -> TokenStream {
    let mut item_fn = parse_macro_input!(item as ItemFn);

    // Force #[no_mangle] so the IR-level symbol name equals the Rust fn name.
    if !item_fn.attrs.iter().any(|a| a.path().is_ident("no_mangle")) {
        item_fn.attrs.push(parse_quote!(#[no_mangle]));
    }
    // Keep the symbol alive under LTO/inlining.
    if !item_fn.attrs.iter().any(|a| a.path().is_ident("inline")) {
        item_fn.attrs.push(parse_quote!(#[inline(never)]));
    }
    // Suppress lint noise stirred up by no_mangle on a possibly-private fn.
    item_fn.attrs.push(parse_quote! {
        #[allow(non_upper_case_globals, dead_code, private_no_mangle_fns,
                exported_private_deps, unused)]
    });

    // Promote visibility to `pub` so no_mangle does not trip
    // private_no_mangle_fns. We only promote inherited (no) visibility; any
    // existing `pub(...)` / `crate` / `super` is left untouched.
    if matches!(item_fn.vis, Visibility::Inherited) {
        item_fn.vis = Visibility::Public(syn::token::Pub::default());
    }

    let fn_ident = item_fn.sig.ident.clone();
    let fn_name = fn_ident.to_string();

    // The marker global symbol must be a literal name so the LLVM pass can
    // find it by exact string match (rustc mangles `static` names by default).
    // We use `#[no_mangle]` on the marker and shape its identifier to be a
    // valid Rust ident: lowercase letters / digits / underscores only.
    //
    // Format: __rt_annot_<fn>_<constraint>[_<arg>]
    //   where <arg> for stack_bound is just the integer (no '=' so the
    //   identifier remains valid).
    let sanitize_ident = |s: &str| -> String {
        s.chars()
            .map(|c| if c.is_ascii_alphanumeric() || c == '_' { c } else { '_' })
            .collect()
    };
    let mut symbol_name = format!("__rt_annot_{}_{}", sanitize_ident(&fn_name), constraint);
    if let Some(a) = &arg {
        // `arg` for stack_bound is "stack_bound=<N>" — strip the leading
        // "stack_bound=" part because '=' is not valid in an ident.
        let arg_part = a
            .split('=')
            .last()
            .map(|s| sanitize_ident(s))
            .unwrap_or_default();
        if !arg_part.is_empty() {
            symbol_name.push('_');
            symbol_name.push_str(&arg_part);
        }
    }
    let marker_id = format_ident!("{}", symbol_name);

    let expanded = quote! {
        #item_fn
        #[used]
        #[no_mangle]
        #[allow(non_upper_case_globals, dead_code, unused)]
        static #marker_id: u8 = 0;
    };
    expanded.into()
}

/// Mark a function as real-time nonblocking. Disallows blocking calls.
#[proc_macro_attribute]
pub fn nonblocking(_attr: TokenStream, item: TokenStream) -> TokenStream {
    expand(item, "nonblocking", None)
}

/// Mark a function as real-time nonallocating. Disallows NormalHeap allocations.
#[proc_macro_attribute]
pub fn nonallocating(_attr: TokenStream, item: TokenStream) -> TokenStream {
    expand(item, "nonallocating", None)
}

/// Mark a function as lock-free. Disallows mutex acquisition.
#[proc_macro_attribute]
pub fn nolock(_attr: TokenStream, item: TokenStream) -> TokenStream {
    expand(item, "nolock", None)
}

/// Mark a function as nothrow. Disallows unwinding/exception propagation.
#[proc_macro_attribute]
pub fn nothrow(_attr: TokenStream, item: TokenStream) -> TokenStream {
    expand(item, "nothrow", None)
}

/// Mark a function as non-recursive.
#[proc_macro_attribute]
pub fn norecurse(_attr: TokenStream, item: TokenStream) -> TokenStream {
    expand(item, "norecurse", None)
}

/// Mark a function as async-signal-safe.
#[proc_macro_attribute]
pub fn async_signal_safe(_attr: TokenStream, item: TokenStream) -> TokenStream {
    expand(item, "async_signal_safe", None)
}

/// Enforce a bounded maximum stack depth (in frames, including this one).
///
/// Usage: `#[rt::stack_bound(64)]` — N must be an integer literal.
#[proc_macro_attribute]
pub fn stack_bound(attr: TokenStream, item: TokenStream) -> TokenStream {
    // Resolve the macro argument to an integer string for the marker suffix.
    let raw = attr.to_string();
    let arg: Option<String> = {
        if raw.trim().is_empty() {
            return syn::Error::new(
                proc_macro2::Span::call_site(),
                "rt::stack_bound requires an integer literal, e.g. #[rt::stack_bound(64)]",
            )
            .to_compile_error()
            .into();
        }
        let digits: String = raw.trim().chars().take_while(|c| c.is_ascii_digit()).collect();
        if digits.is_empty() {
            return syn::Error::new(
                proc_macro2::Span::call_site(),
                "rt::stack_bound requires an integer literal, e.g. #[rt::stack_bound(64)]",
            )
            .to_compile_error()
            .into();
        }
        Some(format!("stack_bound={}", digits))
    };
    expand(item, "stack_bound", arg)
}

