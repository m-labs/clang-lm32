
// RUN: rm -rf %t
// RUN: %clang_cc1 -Wauto-import -fmodule-cache-path %t -fauto-module-import -F %S/Inputs %s -verify

#include <DependsOnModule/DependsOnModule.h> // expected-warning{{treating #include as an import of module 'DependsOnModule'}}

#ifdef MODULE_H_MACRO
#  error MODULE_H_MACRO should have been hidden
#endif

#ifdef DEPENDS_ON_MODULE
#  error DEPENDS_ON_MODULE should have been hidden
#endif

Module *mod; // expected-error{{unknown type name 'Module'}}

#import <AlsoDependsOnModule/AlsoDependsOnModule.h> // expected-warning{{treating #import as an import of module 'AlsoDependsOnModule'}}
Module *mod2;

int getDependsOther() { return depends_on_module_other; }

void testSubframeworkOther() {
  double *sfo1 = sub_framework_other; // expected-error{{use of undeclared identifier 'sub_framework_other'}}
}

// Test header cross-subframework include pattern.
#include <DependsOnModule/../Frameworks/SubFramework.framework/Headers/Other.h> // expected-warning{{treating #include as an import of module 'DependsOnModule.SubFramework.Other'}}

void testSubframeworkOtherAgain() {
  double *sfo1 = sub_framework_other;
}

void testModuleSubFramework() {
  char *msf = module_subframework;
}

#include <Module/../Frameworks/SubFramework.framework/Headers/SubFramework.h> // expected-warning{{treating #include as an import of module 'Module.SubFramework'}}

void testModuleSubFrameworkAgain() {
  char *msf = module_subframework;
}

// Test inclusion of private headers.
#include <DependsOnModule/DependsOnModulePrivate.h> // expected-warning{{treating #include as an import of module 'DependsOnModule.Private.DependsOnModule'}}

int getDependsOnModulePrivate() { return depends_on_module_private; }
