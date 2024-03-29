// RUN: %clang_cc1 %s -triple i686-pc-win32 -fsyntax-only -Wmicrosoft -verify -fms-compatibility -fexceptions -fcxx-exceptions



namespace ms_conversion_rules {

void f(float a);
void f(int a);

void test()
{
    long a = 0;
    f((long)0);
	f(a);
}

}



namespace ms_protected_scope {
  struct C { C(); };

  int jump_over_variable_init(bool b) {
    if (b)
      goto foo; // expected-warning {{goto into protected scope}}
    C c; // expected-note {{jump bypasses variable initialization}}
  foo:
    return 1;
  }

struct Y {
  ~Y();
};

void jump_over_var_with_dtor() {
  goto end; // expected-warning{{goto into protected scope}}
  Y y; // expected-note {{jump bypasses variable with a non-trivial destructor}}
 end:
    ;
}

  void jump_over_variable_case(int c) {
    switch (c) {
    case 0:
      int x = 56; // expected-note {{jump bypasses variable initialization}}
    case 1:       // expected-error {{switch case is in protected scope}}
      x = 10;
    }
  }

 
void exception_jump() {
  goto l2; // expected-error {{goto into protected scope}}
  try { // expected-note {{jump bypasses initialization of try block}}
     l2: ;
  } catch(int) {
  }
}

int jump_over_indirect_goto() {
  static void *ps[] = { &&a0 };
  goto *&&a0; // expected-warning {{goto into protected scope}}
  int a = 3; // expected-note {{jump bypasses variable initialization}}
 a0:
  return 0;
}
  
}



namespace ms_using_declaration_bug {

class A {
public: 
  int f(); 
};

class B : public A {
private:   
  using A::f;
};

class C : public B { 
private:   
  using B::f; // expected-warning {{using declaration referring to inaccessible member 'ms_using_declaration_bug::B::f' (which refers to accessible member 'ms_using_declaration_bug::A::f') is a Microsoft compatibility extension}}
};

}


namespace MissingTypename {

template<class T> class A {
public:
	 typedef int TYPE;
};

template<class T> class B {
public:
	 typedef int TYPE;
};


template<class T, class U>
class C : private A<T>, public B<U> {
public:
   typedef A<T> Base1;
   typedef B<U> Base2;
   typedef A<U> Base3;

   A<T>::TYPE a1; // expected-warning {{missing 'typename' prior to dependent type name}}
   Base1::TYPE a2; // expected-warning {{missing 'typename' prior to dependent type name}}

   B<U>::TYPE a3; // expected-warning {{missing 'typename' prior to dependent type name}}
   Base2::TYPE a4; // expected-warning {{missing 'typename' prior to dependent type name}}

   A<U>::TYPE a5; // expected-error {{missing 'typename' prior to dependent type name}}
   Base3::TYPE a6; // expected-error {{missing 'typename' prior to dependent type name}}
 };

class D {
public:
    typedef int Type;
};

template <class T>
void function_missing_typename(const T::Type param)// expected-warning {{missing 'typename' prior to dependent type name}}
{
    const T::Type var = 2; // expected-warning {{missing 'typename' prior to dependent type name}}
}

template void function_missing_typename<D>(const D::Type param);

}



