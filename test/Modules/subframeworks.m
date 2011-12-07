// RUN: rm -rf %t
// RUN: %clang_cc1 -Wauto-import -fmodule-cache-path %t -fauto-module-import -F %S/Inputs %s -verify

__import_module__ DependsOnModule;

void testSubFramework() {
  float *sf1 = sub_framework; // expected-error{{use of undeclared identifier 'sub_framework'}}
}

__import_module__ DependsOnModule.SubFramework;

void testSubFrameworkAgain() {
  float *sf2 = sub_framework;
  double *sfo1 = sub_framework_other;
}

