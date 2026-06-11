; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-san-place-all" -S %s -o %t.instrumented.ll
; RUN: %clang -fsanitize=realtime %t.instrumented.ll -o %t
; RUN: %t

define void @rt_entry() #0 {
  ret void
}

define i32 @main() {
  call void @rt_entry()
  ret i32 0
}

attributes #0 = { "nonblocking" "nonallocating" }
