// RUN: %clang_cc1 -fobjc-nonfragile-abi -fblocks -fsyntax-only -fobjc-arc -x objective-c %s.result
// RUN: arcmt-test --args -triple x86_64-apple-darwin10 -fblocks -fobjc-nonfragile-abi -fsyntax-only -x objective-c %s > %t
// RUN: diff %t %s.result

typedef int BOOL;

id IhaveSideEffect();

@protocol NSObject
- (BOOL)isEqual:(id)object;
- (id)retain;
- (oneway void)release;
@end

@interface NSObject <NSObject> {}
@end

@interface Foo : NSObject {
  id bar;
}
@property (retain) id bar;
-(void)test:(id)obj;
@end

@implementation Foo

@synthesize bar;

-(void)test:(id)obj {
  id x = self.bar;
  [x retain];
  self.bar = obj;
  // do stuff with x;
  [x release];

  [IhaveSideEffect() release];

  [x release], x = 0;
}
  
@end

void func(Foo *p) {
  [p release];
  (([p release]));
}

@interface Baz {
	id <NSObject> _foo;
}
@end

@implementation Baz
- dealloc {
  [_foo release];
  return 0;
}
@end

void block_test(Foo *p) {
  id (^B)() = ^() {
    if (p) {
      id (^IB)() = ^() {
        id bar = [p retain];
	      [p release];
        return bar;
      };
      IB();
    }
    return [p retain];
  };
}

#define RELEASE_MACRO(x) [x release]
#define RELEASE_MACRO2(x) RELEASE_MACRO(x)

void test2(id p) {
  RELEASE_MACRO(p);
  RELEASE_MACRO2(p);
}
