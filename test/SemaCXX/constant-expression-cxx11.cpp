// RUN: %clang_cc1 -triple i686-linux -fsyntax-only -verify -std=c++11 %s

// This version of static_assert just requires a foldable value as the
// expression, not an ICE.
// FIXME: Once we implement the C++11 ICE rules, most uses of this here should
// be converted to static_assert.
#define static_assert_fold(expr, str) \
    static_assert(__builtin_constant_p(expr), "not an integral constant expression"); \
    static_assert(__builtin_constant_p(expr) ? expr : true, str)

namespace StaticAssertFoldTest {

int x;
static_assert_fold(++x, "test"); // expected-error {{not an integral constant expression}}
static_assert_fold(false, "test"); // expected-error {{test}}

}

// FIXME: support const T& parameters here.
//template<typename T> constexpr T id(const T &t) { return t; }
template<typename T> constexpr T id(T t) { return t; }
// FIXME: support templates here.
//template<typename T> constexpr T min(const T &a, const T &b) {
//  return a < b ? a : b;
//}
//template<typename T> constexpr T max(const T &a, const T &b) {
//  return a < b ? b : a;
//}
constexpr int min(const int &a, const int &b) { return a < b ? a : b; }
constexpr int max(const int &a, const int &b) { return a < b ? b : a; }

struct MemberZero {
  constexpr int zero() { return 0; }
};

namespace DerivedToVBaseCast {

  struct U { int n; };
  struct V : U { int n; };
  struct A : virtual V { int n; };
  struct Aa { int n; };
  struct B : virtual A, Aa {};
  struct C : virtual A, Aa {};
  struct D : B, C {};

  D d;
  constexpr B *p = &d;
  constexpr C *q = &d;
  static_assert_fold((void*)p != (void*)q, "");
  static_assert_fold((A*)p == (A*)q, "");
  static_assert_fold((Aa*)p != (Aa*)q, "");

  constexpr B &pp = d;
  constexpr C &qq = d;
  static_assert_fold((void*)&pp != (void*)&qq, "");
  static_assert_fold(&(A&)pp == &(A&)qq, "");
  static_assert_fold(&(Aa&)pp != &(Aa&)qq, "");

  constexpr V *v = p;
  constexpr V *w = q;
  constexpr V *x = (A*)p;
  static_assert_fold(v == w, "");
  static_assert_fold(v == x, "");

  static_assert_fold((U*)&d == p, "");
  static_assert_fold((U*)&d == q, "");
  static_assert_fold((U*)&d == v, "");
  static_assert_fold((U*)&d == w, "");
  static_assert_fold((U*)&d == x, "");

  struct X {};
  struct Y1 : virtual X {};
  struct Y2 : X {};
  struct Z : Y1, Y2 {};
  Z z;
  static_assert_fold((X*)(Y1*)&z != (X*)(Y2*)&z, "");

}

namespace ConstCast {

constexpr int n1 = 0;
constexpr int n2 = const_cast<int&>(n1);
constexpr int *n3 = const_cast<int*>(&n1);
constexpr int n4 = *const_cast<int*>(&n1);
constexpr const int * const *n5 = const_cast<const int* const*>(&n3);
constexpr int **n6 = const_cast<int**>(&n3);
constexpr int n7 = **n5;
constexpr int n8 = **n6;

}

namespace TemplateArgumentConversion {
  template<int n> struct IntParam {};

  using IntParam0 = IntParam<0>;
  // FIXME: This should be accepted once we implement the new ICE rules.
  using IntParam0 = IntParam<id(0)>; // expected-error {{not an integral constant expression}}
  using IntParam0 = IntParam<MemberZero().zero>; // expected-error {{did you mean to call it with no arguments?}} expected-error {{not an integral constant expression}}
}

namespace CaseStatements {
  void f(int n) {
    switch (n) {
    // FIXME: Produce the 'add ()' fixit for this.
    case MemberZero().zero: // desired-error {{did you mean to call it with no arguments?}} expected-error {{not an integer constant expression}}
    // FIXME: This should be accepted once we implement the new ICE rules.
    case id(1): // expected-error {{not an integer constant expression}}
      return;
    }
  }
}

extern int &Recurse1;
int &Recurse2 = Recurse1, &Recurse1 = Recurse2;
constexpr int &Recurse3 = Recurse2; // expected-error {{must be initialized by a constant expression}}

namespace MemberEnum {
  struct WithMemberEnum {
    enum E { A = 42 };
  } wme;

  static_assert_fold(wme.A == 42, "");
}

namespace DefaultArguments {

const int z = int();
constexpr int Sum(int a = 0, const int &b = 0, const int *c = &z, char d = 0) {
  return a + b + *c + d;
}
const int four = 4;
constexpr int eight = 8;
constexpr const int twentyseven = 27;
static_assert_fold(Sum() == 0, "");
static_assert_fold(Sum(1) == 1, "");
static_assert_fold(Sum(1, four) == 5, "");
static_assert_fold(Sum(1, eight, &twentyseven) == 36, "");
static_assert_fold(Sum(1, 2, &four, eight) == 15, "");

}

namespace Ellipsis {

// Note, values passed through an ellipsis can't actually be used.
constexpr int F(int a, ...) { return a; }
static_assert_fold(F(0) == 0, "");
static_assert_fold(F(1, 0) == 1, "");
static_assert_fold(F(2, "test") == 2, "");
static_assert_fold(F(3, &F) == 3, "");
int k = 0;
static_assert_fold(F(4, k) == 3, ""); // expected-error {{constant expression}}

}

namespace Recursion {
  constexpr int fib(int n) { return n > 1 ? fib(n-1) + fib(n-2) : n; }
  static_assert_fold(fib(11) == 89, "");

  constexpr int gcd_inner(int a, int b) {
    return b == 0 ? a : gcd_inner(b, a % b);
  }
  constexpr int gcd(int a, int b) {
    return gcd_inner(max(a, b), min(a, b));
  }

  static_assert_fold(gcd(1749237, 5628959) == 7, "");
}

namespace FunctionCast {
  // When folding, we allow functions to be cast to different types. Such
  // cast functions cannot be called, even if they're constexpr.
  constexpr int f() { return 1; }
  typedef double (*DoubleFn)();
  typedef int (*IntFn)();
  int a[(int)DoubleFn(f)()]; // expected-error {{variable length array}}
  int b[(int)IntFn(f)()];    // ok
}

namespace StaticMemberFunction {
  struct S {
    static constexpr int k = 42;
    static constexpr int f(int n) { return n * k + 2; }
  } s;

  constexpr int n = s.f(19);
  static_assert_fold(S::f(19) == 800, "");
  static_assert_fold(s.f(19) == 800, "");
  static_assert_fold(n == 800, "");

  constexpr int (*sf1)(int) = &S::f;
  constexpr int (*sf2)(int) = &s.f;
  constexpr const int *sk = &s.k;
}

namespace ParameterScopes {

  const int k = 42;
  constexpr const int &ObscureTheTruth(const int &a) { return a; }
  constexpr const int &MaybeReturnJunk(bool b, const int a) {
    return ObscureTheTruth(b ? a : k);
  }
  static_assert_fold(MaybeReturnJunk(false, 0) == 42, ""); // ok
  constexpr int a = MaybeReturnJunk(true, 0); // expected-error {{constant expression}}

  constexpr const int MaybeReturnNonstaticRef(bool b, const int a) {
    // If ObscureTheTruth returns a reference to 'a', the result is not a
    // constant expression even though 'a' is still in scope.
    return ObscureTheTruth(b ? a : k);
  }
  static_assert_fold(MaybeReturnNonstaticRef(false, 0) == 42, ""); // ok
  constexpr int b = MaybeReturnNonstaticRef(true, 0); // expected-error {{constant expression}}

  constexpr int InternalReturnJunk(int n) {
    // FIXME: We should reject this: it never produces a constant expression.
    return MaybeReturnJunk(true, n);
  }
  constexpr int n3 = InternalReturnJunk(0); // expected-error {{must be initialized by a constant expression}}

  constexpr int LToR(int &n) { return n; }
  constexpr int GrabCallersArgument(bool which, int a, int b) {
    return LToR(which ? b : a);
  }
  static_assert_fold(GrabCallersArgument(false, 1, 2) == 1, "");
  static_assert_fold(GrabCallersArgument(true, 4, 8) == 8, "");

}

namespace Pointers {

  constexpr int f(int n, const int *a, const int *b, const int *c) {
    return n == 0 ? 0 : *a + f(n-1, b, c, a);
  }

  const int x = 1, y = 10, z = 100;
  static_assert_fold(f(23, &x, &y, &z) == 788, "");

  constexpr int g(int n, int a, int b, int c) {
    return f(n, &a, &b, &c);
  }
  static_assert_fold(g(23, x, y, z) == 788, "");

}

namespace FunctionPointers {

  constexpr int Double(int n) { return 2 * n; }
  constexpr int Triple(int n) { return 3 * n; }
  constexpr int Twice(int (*F)(int), int n) { return F(F(n)); }
  constexpr int Quadruple(int n) { return Twice(Double, n); }
  constexpr auto Select(int n) -> int (*)(int) {
    return n == 2 ? &Double : n == 3 ? &Triple : n == 4 ? &Quadruple : 0;
  }
  constexpr int Apply(int (*F)(int), int n) { return F(n); }

  static_assert_fold(1 + Apply(Select(4), 5) + Apply(Select(3), 7) == 42, "");

  constexpr int Invalid = Apply(Select(0), 0); // expected-error {{must be initialized by a constant expression}}

}

namespace PointerComparison {

int x, y;
static_assert_fold(&x == &y, "false"); // expected-error {{false}}
static_assert_fold(&x != &y, "");
constexpr bool g1 = &x == &y;
constexpr bool g2 = &x != &y;
constexpr bool g3 = &x <= &y; // expected-error {{must be initialized by a constant expression}}
constexpr bool g4 = &x >= &y; // expected-error {{must be initialized by a constant expression}}
constexpr bool g5 = &x < &y; // expected-error {{must be initialized by a constant expression}}
constexpr bool g6 = &x > &y; // expected-error {{must be initialized by a constant expression}}

struct S { int x, y; } s;
static_assert_fold(&s.x == &s.y, "false"); // expected-error {{false}}
static_assert_fold(&s.x != &s.y, "");
static_assert_fold(&s.x <= &s.y, "");
static_assert_fold(&s.x >= &s.y, "false"); // expected-error {{false}}
static_assert_fold(&s.x < &s.y, "");
static_assert_fold(&s.x > &s.y, "false"); // expected-error {{false}}

static_assert_fold(0 == &y, "false"); // expected-error {{false}}
static_assert_fold(0 != &y, "");
constexpr bool n3 = 0 <= &y; // expected-error {{must be initialized by a constant expression}}
constexpr bool n4 = 0 >= &y; // expected-error {{must be initialized by a constant expression}}
constexpr bool n5 = 0 < &y; // expected-error {{must be initialized by a constant expression}}
constexpr bool n6 = 0 > &y; // expected-error {{must be initialized by a constant expression}}

static_assert_fold(&x == 0, "false"); // expected-error {{false}}
static_assert_fold(&x != 0, "");
constexpr bool n9 = &x <= 0; // expected-error {{must be initialized by a constant expression}}
constexpr bool n10 = &x >= 0; // expected-error {{must be initialized by a constant expression}}
constexpr bool n11 = &x < 0; // expected-error {{must be initialized by a constant expression}}
constexpr bool n12 = &x > 0; // expected-error {{must be initialized by a constant expression}}

static_assert_fold(&x == &x, "");
static_assert_fold(&x != &x, "false"); // expected-error {{false}}
static_assert_fold(&x <= &x, "");
static_assert_fold(&x >= &x, "");
static_assert_fold(&x < &x, "false"); // expected-error {{false}}
static_assert_fold(&x > &x, "false"); // expected-error {{false}}

constexpr S* sptr = &s;
// FIXME: This is not a constant expression; check we reject this and move this
// test elsewhere.
constexpr bool dyncast = sptr == dynamic_cast<S*>(sptr);

extern char externalvar[];
// FIXME: This is not a constant expression; check we reject this and move this
// test elsewhere.
constexpr bool constaddress = (void *)externalvar == (void *)0x4000UL;  // expected-error {{must be initialized by a constant expression}}
constexpr bool litaddress = "foo" == "foo"; // expected-error {{must be initialized by a constant expression}} expected-warning {{unspecified}}
static_assert_fold(0 != "foo", "");

}

namespace MaterializeTemporary {

constexpr int f(const int &r) { return r; }
constexpr int n = f(1);

constexpr bool same(const int &a, const int &b) { return &a == &b; }
constexpr bool sameTemporary(const int &n) { return same(n, n); }

static_assert_fold(n, "");
static_assert_fold(!same(4, 4), "");
static_assert_fold(same(n, n), "");
static_assert_fold(sameTemporary(9), "");

}

constexpr int strcmp_ce(const char *p, const char *q) {
  return (!*p || *p != *q) ? *p - *q : strcmp_ce(p+1, q+1);
}

namespace StringLiteral {

// FIXME: Refactor this once we support constexpr templates.
constexpr int MangleChars(const char *p) {
  return *p + 3 * (*p ? MangleChars(p+1) : 0);
}
constexpr int MangleChars(const char16_t *p) {
  return *p + 3 * (*p ? MangleChars(p+1) : 0);
}
constexpr int MangleChars(const char32_t *p) {
  return *p + 3 * (*p ? MangleChars(p+1) : 0);
}

static_assert_fold(MangleChars("constexpr!") == 1768383, "");
static_assert_fold(MangleChars(u"constexpr!") == 1768383, "");
static_assert_fold(MangleChars(U"constexpr!") == 1768383, "");

constexpr char c0 = "nought index"[0];
constexpr char c1 = "nice index"[10];
constexpr char c2 = "nasty index"[12]; // expected-error {{must be initialized by a constant expression}} expected-warning {{is past the end}}
constexpr char c3 = "negative index"[-1]; // expected-error {{must be initialized by a constant expression}} expected-warning {{is before the beginning}}
constexpr char c4 = ((char*)(int*)"no reinterpret_casts allowed")[14]; // expected-error {{must be initialized by a constant expression}}

constexpr const char *p = "test" + 2;
static_assert_fold(*p == 's', "");

constexpr const char *max_iter(const char *a, const char *b) {
  return *a < *b ? b : a;
}
constexpr const char *max_element(const char *a, const char *b) {
  return (a+1 >= b) ? a : max_iter(a, max_element(a+1, b));
}

constexpr const char *begin(const char (&arr)[45]) { return arr; }
constexpr const char *end(const char (&arr)[45]) { return arr + 45; }

constexpr char str[] = "the quick brown fox jumped over the lazy dog";
constexpr const char *max = max_element(begin(str), end(str));
static_assert_fold(*max == 'z', "");
static_assert_fold(max == str + 38, "");

static_assert_fold(strcmp_ce("hello world", "hello world") == 0, "");
static_assert_fold(strcmp_ce("hello world", "hello clang") > 0, "");
static_assert_fold(strcmp_ce("constexpr", "test") < 0, "");
static_assert_fold(strcmp_ce("", " ") < 0, "");

}

namespace Array {

// FIXME: Use templates for these once we support constexpr templates.
constexpr int Sum(const int *begin, const int *end) {
  return begin == end ? 0 : *begin + Sum(begin+1, end);
}
constexpr const int *begin(const int (&xs)[5]) { return xs; }
constexpr const int *end(const int (&xs)[5]) { return xs + 5; }

constexpr int xs[] = { 1, 2, 3, 4, 5 };
constexpr int ys[] = { 5, 4, 3, 2, 1 };
constexpr int sum_xs = Sum(begin(xs), end(xs));
static_assert_fold(sum_xs == 15, "");

constexpr int ZipFoldR(int (*F)(int x, int y, int c), int n,
                       const int *xs, const int *ys, int c) {
  return n ? F(*xs, *ys, ZipFoldR(F, n-1, xs+1, ys+1, c)) : c;
}
constexpr int MulAdd(int x, int y, int c) { return x * y + c; }
constexpr int InnerProduct = ZipFoldR(MulAdd, 5, xs, ys, 0);
static_assert_fold(InnerProduct == 35, "");

constexpr int SubMul(int x, int y, int c) { return (x - y) * c; }
constexpr int DiffProd = ZipFoldR(SubMul, 2, xs+3, ys+3, 1);
static_assert_fold(DiffProd == 8, "");
static_assert_fold(ZipFoldR(SubMul, 3, xs+3, ys+3, 1), ""); // expected-error {{constant expression}}

constexpr const int *p = xs + 3;
constexpr int xs4 = p[1]; // ok
constexpr int xs5 = p[2]; // expected-error {{constant expression}}
constexpr int xs0 = p[-3]; // ok
constexpr int xs_1 = p[-4]; // expected-error {{constant expression}}

constexpr int zs[2][2][2][2] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static_assert_fold(zs[0][0][0][0] == 1, "");
static_assert_fold(zs[1][1][1][1] == 16, "");
static_assert_fold(zs[0][0][0][2] == 3, ""); // expected-error {{constant expression}}
static_assert_fold((&zs[0][0][0][2])[-1] == 2, "");
static_assert_fold(**(**(zs + 1) + 1) == 11, "");
static_assert_fold(*(&(&(*(*&(&zs[2] - 1)[0] + 2 - 2))[2])[-1][-1] + 1) == 11, "");

constexpr int arr[40] = { 1, 2, 3, [8] = 4 };
constexpr int SumNonzero(const int *p) {
  return *p + (*p ? SumNonzero(p+1) : 0);
}
constexpr int CountZero(const int *p, const int *q) {
  return p == q ? 0 : (*p == 0) + CountZero(p+1, q);
}
static_assert_fold(SumNonzero(arr) == 6, "");
static_assert_fold(CountZero(arr, arr + 40) == 36, "");

struct ArrayElem {
  constexpr ArrayElem() : n(0) {}
  int n;
  constexpr int f() { return n; }
};
struct ArrayRVal {
  constexpr ArrayRVal() {}
  ArrayElem elems[10];
};
static_assert_fold(ArrayRVal().elems[3].f() == 0, "");

}

namespace DependentValues {

struct I { int n; typedef I V[10]; };
I::V x, y;
template<bool B> struct S {
  int k;
  void f() {
    I::V &cells = B ? x : y;
    I &i = cells[k];
    switch (i.n) {}
  }
};

}

namespace Class {

struct A { constexpr A(int a, int b) : k(a + b) {} int k; };
constexpr int fn(const A &a) { return a.k; }
static_assert_fold(fn(A(4,5)) == 9, "");

struct B { int n; int m; } constexpr b = { 0, b.n }; // expected-warning {{uninitialized}}
struct C {
  constexpr C(C *this_) : m(42), n(this_->m) {} // ok
  int m, n;
};
struct D {
  C c;
  constexpr D() : c(&c) {}
};
static_assert_fold(D().c.n == 42, "");

struct E {
  constexpr E() : p(&p) {}
  void *p;
};
constexpr const E &e1 = E(); // expected-error {{constant expression}}
// This is a constant expression if we elide the copy constructor call, and
// is not a constant expression if we don't! But we do, so it is.
// FIXME: The move constructor is not currently implicitly defined as constexpr.
// We notice this when evaluating an expression which uses it, but not when
// checking its initializer.
constexpr E e2 = E(); // unexpected-error {{constant expression}}
static_assert_fold(e2.p == &e2.p, ""); // unexpected-error {{constant expression}}
// FIXME: We don't pass through the fact that 'this' is ::e3 when checking the
// initializer of this declaration.
constexpr E e3; // unexpected-error {{constant expression}}
static_assert_fold(e3.p == &e3.p, "");

extern const class F f;
struct F {
  constexpr F() : p(&f.p) {}
  const void *p;
};
constexpr F f = F();

struct G {
  struct T {
    constexpr T(T *p) : u1(), u2(p) {}
    union U1 {
      constexpr U1() {}
      int a, b = 42;
    } u1;
    union U2 {
      constexpr U2(T *p) : c(p->u1.b) {}
      int c, d;
    } u2;
  } t;
  constexpr G() : t(&t) {}
} constexpr g;

static_assert_fold(g.t.u1.a == 42, ""); // expected-error {{constant expression}}
static_assert_fold(g.t.u1.b == 42, "");
static_assert_fold(g.t.u2.c == 42, "");
static_assert_fold(g.t.u2.d == 42, ""); // expected-error {{constant expression}}

struct S {
  int a, b;
  const S *p;
  double d;
  const char *q;

  constexpr S(int n, const S *p) : a(5), b(n), p(p), d(n), q("hello") {}
};

S global(43, &global);

static_assert_fold(S(15, &global).b == 15, "");

constexpr bool CheckS(const S &s) {
  return s.a == 5 && s.b == 27 && s.p == &global && s.d == 27. && s.q[3] == 'l';
}
static_assert_fold(CheckS(S(27, &global)), "");

struct Arr {
  char arr[3];
  constexpr Arr() : arr{'x', 'y', 'z'} {}
};
constexpr int hash(Arr &&a) {
  return a.arr[0] + a.arr[1] * 0x100 + a.arr[2] * 0x10000;
}
constexpr int k = hash(Arr());
static_assert_fold(k == 0x007a7978, "");


struct AggregateInit {
  const char &c;
  int n;
  double d;
  int arr[5];
  void *p;
};

constexpr AggregateInit agg1 = { "hello"[0] };

static_assert_fold(strcmp_ce(&agg1.c, "hello") == 0, "");
static_assert_fold(agg1.n == 0, "");
static_assert_fold(agg1.d == 0.0, "");
static_assert_fold(agg1.arr[-1] == 0, ""); // expected-error {{constant expression}}
static_assert_fold(agg1.arr[0] == 0, "");
static_assert_fold(agg1.arr[4] == 0, "");
static_assert_fold(agg1.arr[5] == 0, ""); // expected-error {{constant expression}}
static_assert_fold(agg1.p == nullptr, "");

namespace SimpleDerivedClass {

struct B {
  constexpr B(int n) : a(n) {}
  int a;
};
struct D : B {
  constexpr D(int n) : B(n) {}
};
constexpr D d(3);
static_assert_fold(d.a == 3, "");

}

struct Bottom { constexpr Bottom() {} };
struct Base : Bottom {
  constexpr Base(int a = 42, const char *b = "test") : a(a), b(b) {}
  int a;
  const char *b;
};
struct Base2 : Bottom {
  constexpr Base2(const int &r) : r(r) {}
  int q = 123;
  // FIXME: When we track the global for which we are computing the initializer,
  // use a reference here.
  //const int &r;
  int r;
};
struct Derived : Base, Base2 {
  constexpr Derived() : Base(76), Base2(a) {}
  int c = r + b[1];
};

constexpr bool operator==(const Base &a, const Base &b) {
  return a.a == b.a && strcmp_ce(a.b, b.b) == 0;
}

constexpr Base base;
constexpr Base base2(76);
constexpr Derived derived;
static_assert_fold(derived.a == 76, "");
static_assert_fold(derived.b[2] == 's', "");
static_assert_fold(derived.c == 76 + 'e', "");
static_assert_fold(derived.q == 123, "");
static_assert_fold(derived.r == 76, "");
static_assert_fold(&derived.r == &derived.a, ""); // expected-error {{}}

static_assert_fold(!(derived == base), "");
static_assert_fold(derived == base2, "");

constexpr Bottom &bot1 = (Base&)derived;
constexpr Bottom &bot2 = (Base2&)derived;
static_assert_fold(&bot1 != &bot2, "");

constexpr Bottom *pb1 = (Base*)&derived;
constexpr Bottom *pb2 = (Base2*)&derived;
static_assert_fold(pb1 != pb2, "");
static_assert_fold(pb1 == &bot1, "");
static_assert_fold(pb2 == &bot2, "");

constexpr Base2 &fail = (Base2&)bot1; // expected-error {{constant expression}}
constexpr Base &fail2 = (Base&)*pb2; // expected-error {{constant expression}}
constexpr Base2 &ok2 = (Base2&)bot2;
static_assert_fold(&ok2 == &derived, "");

constexpr Base2 *pfail = (Base2*)pb1; // expected-error {{constant expression}}
constexpr Base *pfail2 = (Base*)&bot2; // expected-error {{constant expression}}
constexpr Base2 *pok2 = (Base2*)pb2;
static_assert_fold(pok2 == &derived, "");
static_assert_fold(&ok2 == pok2, "");
static_assert_fold((Base2*)(Derived*)(Base*)pb1 == pok2, "");
static_assert_fold((Derived*)(Base*)pb1 == (Derived*)pok2, "");

constexpr Base *nullB = 42 - 6 * 7;
static_assert_fold((Bottom*)nullB == 0, "");
static_assert_fold((Derived*)nullB == 0, "");
static_assert_fold((void*)(Bottom*)nullB == (void*)(Derived*)nullB, "");

}

namespace Temporaries {

struct S {
  constexpr S() {}
  constexpr int f();
};
struct T : S {
  constexpr T(int n) : S(), n(n) {}
  int n;
};
constexpr int S::f() {
  // 'this' must be the postfix-expression in a class member access expression,
  // so we can't just use
  //   return static_cast<T*>(this)->n;
  return this->*(int(S::*))&T::n;
}
// The T temporary is implicitly cast to an S subobject, but we can recover the
// T full-object via a base-to-derived cast, or a derived-to-base-casted member
// pointer.
static_assert_fold(T(3).f() == 3, "");

constexpr int f(const S &s) {
  return static_cast<const T&>(s).n;
}
constexpr int n = f(T(5));
static_assert_fold(f(T(5)) == 5, "");

}

namespace Union {

union U {
  int a;
  int b;
};

constexpr U u[4] = { { .a = 0 }, { .b = 1 }, { .a = 2 }, { .b = 3 } };
static_assert_fold(u[0].a == 0, "");
static_assert_fold(u[0].b, ""); // expected-error {{constant expression}}
static_assert_fold(u[1].b == 1, "");
static_assert_fold((&u[1].b)[1] == 2, ""); // expected-error {{constant expression}}
static_assert_fold(*(&(u[1].b) + 1 + 1) == 3, ""); // expected-error {{constant expression}}
static_assert_fold((&(u[1]) + 1 + 1)->b == 3, "");

}

namespace MemberPointer {
  struct A {
    constexpr A(int n) : n(n) {}
    int n;
    constexpr int f() { return n + 3; }
  };
  constexpr A a(7);
  static_assert_fold(A(5).*&A::n == 5, "");
  static_assert_fold((&a)->*&A::n == 7, "");
  static_assert_fold((A(8).*&A::f)() == 11, "");
  static_assert_fold(((&a)->*&A::f)() == 10, "");

  struct B : A {
    constexpr B(int n, int m) : A(n), m(m) {}
    int m;
    constexpr int g() { return n + m + 1; }
  };
  constexpr B b(9, 13);
  static_assert_fold(B(4, 11).*&A::n == 4, "");
  static_assert_fold(B(4, 11).*&B::m == 11, "");
  static_assert_fold(B(4, 11).*(int(A::*))&B::m == 11, "");
  static_assert_fold((&b)->*&A::n == 9, "");
  static_assert_fold((&b)->*&B::m == 13, "");
  static_assert_fold((&b)->*(int(A::*))&B::m == 13, "");
  static_assert_fold((B(4, 11).*&A::f)() == 7, "");
  static_assert_fold((B(4, 11).*&B::g)() == 16, "");
  static_assert_fold((B(4, 11).*(int(A::*)()const)&B::g)() == 16, "");
  static_assert_fold(((&b)->*&A::f)() == 12, "");
  static_assert_fold(((&b)->*&B::g)() == 23, "");
  static_assert_fold(((&b)->*(int(A::*)()const)&B::g)() == 23, "");

  struct S {
    constexpr S(int m, int n, int (S::*pf)() const, int S::*pn) :
      m(m), n(n), pf(pf), pn(pn) {}
    constexpr S() : m(), n(), pf(&S::f), pn(&S::n) {}

    constexpr int f() { return this->*pn; }
    virtual int g() const;

    int m, n;
    int (S::*pf)() const;
    int S::*pn;
  };

  constexpr int S::*pm = &S::m;
  constexpr int S::*pn = &S::n;
  constexpr int (S::*pf)() const = &S::f;
  constexpr int (S::*pg)() const = &S::g;

  constexpr S s(2, 5, &S::f, &S::m);

  static_assert_fold((s.*&S::f)() == 2, "");
  static_assert_fold((s.*s.pf)() == 2, "");

  template<int n> struct T : T<n-1> {};
  template<> struct T<0> { int n; };
  template<> struct T<30> : T<29> { int m; };

  T<17> t17;
  T<30> t30;

  constexpr int (T<10>::*deepn) = &T<0>::n;
  static_assert_fold(&(t17.*deepn) == &t17.n, "");

  constexpr int (T<15>::*deepm) = (int(T<10>::*))&T<30>::m;
  constexpr int *pbad = &(t17.*deepm); // expected-error {{constant expression}}
  static_assert_fold(&(t30.*deepm) == &t30.m, "");

  constexpr T<5> *p17_5 = &t17;
  constexpr T<13> *p17_13 = (T<13>*)p17_5;
  constexpr T<23> *p17_23 = (T<23>*)p17_13; // expected-error {{constant expression}}
  static_assert_fold(&(p17_5->*(int(T<3>::*))deepn) == &t17.n, "");
  static_assert_fold(&(p17_13->*deepn) == &t17.n, "");
  constexpr int *pbad2 = &(p17_13->*(int(T<9>::*))deepm); // expected-error {{constant expression}}

  constexpr T<5> *p30_5 = &t30;
  constexpr T<23> *p30_23 = (T<23>*)p30_5;
  constexpr T<13> *p30_13 = p30_23;
  static_assert_fold(&(p30_5->*(int(T<3>::*))deepn) == &t30.n, "");
  static_assert_fold(&(p30_13->*deepn) == &t30.n, "");
  static_assert_fold(&(p30_23->*deepn) == &t30.n, "");
  static_assert_fold(&(p30_5->*(int(T<2>::*))deepm) == &t30.m, "");
  static_assert_fold(&(((T<17>*)p30_13)->*deepm) == &t30.m, "");
  static_assert_fold(&(p30_23->*deepm) == &t30.m, "");
}

namespace ArrayBaseDerived {

  struct Base {
    constexpr Base() {}
    int n = 0;
  };
  struct Derived : Base {
    constexpr Derived() {}
    constexpr const int *f() { return &n; }
  };

  constexpr Derived a[10];
  constexpr Derived *pd3 = const_cast<Derived*>(&a[3]);
  constexpr Base *pb3 = const_cast<Derived*>(&a[3]);
  static_assert_fold(pb3 == pd3, "");

  // pb3 does not point to an array element.
  constexpr Base *pb4 = pb3 + 1; // ok, one-past-the-end pointer.
  constexpr int pb4n = pb4->n; // expected-error {{constant expression}}
  constexpr Base *err_pb5 = pb3 + 2; // FIXME: reject this.
  constexpr int err_pb5n = err_pb5->n; // expected-error {{constant expression}}
  constexpr Base *err_pb2 = pb3 - 1; // FIXME: reject this.
  constexpr int err_pb2n = err_pb2->n; // expected-error {{constant expression}}
  constexpr Base *pb3a = pb4 - 1;

  // pb4 does not point to a Derived.
  constexpr Derived *err_pd4 = (Derived*)pb4; // expected-error {{constant expression}}
  constexpr Derived *pd3a = (Derived*)pb3a;
  constexpr int pd3n = pd3a->n;

  // pd3a still points to the Derived array.
  constexpr Derived *pd6 = pd3a + 3;
  static_assert_fold(pd6 == &a[6], "");
  constexpr Derived *pd9 = pd6 + 3;
  constexpr Derived *pd10 = pd6 + 4;
  constexpr int pd9n = pd9->n; // ok
  constexpr int err_pd10n = pd10->n; // expected-error {{constant expression}}
  constexpr int pd0n = pd10[-10].n;
  constexpr int err_pdminus1n = pd10[-11].n; // expected-error {{constant expression}}

  constexpr Base *pb9 = pd9;
  constexpr const int *(Base::*pfb)() const =
      static_cast<const int *(Base::*)() const>(&Derived::f);
  static_assert_fold((pb9->*pfb)() == &a[9].n, "");
}

namespace Complex {

class complex {
  int re, im;
public:
  constexpr complex(int re = 0, int im = 0) : re(re), im(im) {}
  constexpr complex(const complex &o) : re(o.re), im(o.im) {}
  constexpr complex operator-() const { return complex(-re, -im); }
  friend constexpr complex operator+(const complex &l, const complex &r) {
    return complex(l.re + r.re, l.im + r.im);
  }
  friend constexpr complex operator-(const complex &l, const complex &r) {
    return l + -r;
  }
  friend constexpr complex operator*(const complex &l, const complex &r) {
    return complex(l.re * r.re - l.im * r.im, l.re * r.im + l.im * r.re);
  }
  friend constexpr bool operator==(const complex &l, const complex &r) {
    return l.re == r.re && l.im == r.im;
  }
  constexpr bool operator!=(const complex &r) const {
    return re != r.re || im != r.im;
  }
  constexpr int real() const { return re; }
  constexpr int imag() const { return im; }
};

constexpr complex i = complex(0, 1);
constexpr complex k = (3 + 4*i) * (6 - 4*i);
static_assert_fold(complex(1,0).real() == 1, "");
static_assert_fold(complex(1,0).imag() == 0, "");
static_assert_fold(((complex)1).imag() == 0, "");
static_assert_fold(k.real() == 34, "");
static_assert_fold(k.imag() == 12, "");
static_assert_fold(k - 34 == 12*i, "");
static_assert_fold((complex)1 == complex(1), "");
static_assert_fold((complex)1 != complex(0, 1), "");
static_assert_fold(complex(1) == complex(1), "");
static_assert_fold(complex(1) != complex(0, 1), "");
constexpr complex makeComplex(int re, int im) { return complex(re, im); }
static_assert_fold(makeComplex(1,0) == complex(1), "");
static_assert_fold(makeComplex(1,0) != complex(0, 1), "");

class complex_wrap : public complex {
public:
  constexpr complex_wrap(int re, int im = 0) : complex(re, im) {}
  constexpr complex_wrap(const complex_wrap &o) : complex(o) {}
};

static_assert_fold((complex_wrap)1 == complex(1), "");
static_assert_fold((complex)1 != complex_wrap(0, 1), "");
static_assert_fold(complex(1) == complex_wrap(1), "");
static_assert_fold(complex_wrap(1) != complex(0, 1), "");
constexpr complex_wrap makeComplexWrap(int re, int im) {
  return complex_wrap(re, im);
}
static_assert_fold(makeComplexWrap(1,0) == complex(1), "");
static_assert_fold(makeComplexWrap(1,0) != complex(0, 1), "");

}
