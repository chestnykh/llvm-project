// RUN: %clang -### -fsyntax-only -c %s 2>&1 | FileCheck %s --check-prefix=CHECK-C
// RUN: %clang -### -fsyntax-only -S %s 2>&1 | FileCheck %s --check-prefix=CHECK-S
// RUN: %clang -### -fsyntax-only -c -S %s 2>&1 | FileCheck %s --check-prefix=CHECK-CS

// CHECK-C: clang: warning: argument unused during compilation: '-c' [-Wunused-command-line-argument]
// CHECK-S: clang: warning: argument unused during compilation: '-S' [-Wunused-command-line-argument]

// CHECK-CS: clang: warning: argument unused during compilation: '-c' [-Wunused-command-line-argument]
// CHECK-CS: clang: warning: argument unused during compilation: '-S' [-Wunused-command-line-argument]
