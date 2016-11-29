// RUN: %llvmgcc %s -emit-llvm -O0 -g -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out --exit-on-error %t1.bc > %t-output.txt 2>&1
// RUN: FileCheck -input-file=%t-output.txt %s
#include "klee/klee.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

int main() {
  float x = 100.0f;
  float result = klee_sqrt_float(x);
  printf("sqrt(%f) = %f\n", x, result);
  assert(result == 10.f);

  x = -0.0f;
  assert(signbit(x));
  result = klee_sqrt_float(x);
  printf("sqrt(%f) = %f\n", x, result);
  assert(result == -0.0f);
  assert(signbit(result));

  printf("Test sqrt negative\n");
  x = -FLT_MIN;
  assert(signbit(x));
  assert(x < 0.0f);
  result = klee_sqrt_float(x);
  printf("sqrt(%f) = %f\n", x, result);
  assert(isnan(result));

  return 0;
}
// CHECK: KLEE: done: completed paths = 1
