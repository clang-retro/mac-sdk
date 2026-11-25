/* Checks the C++ runtime (runtime/build-libstdcxx.sh): static
 * constructors and destructors, exceptions (thrown through frames with
 * destructors, caught by type, by base class, by catch (...), rethrown,
 * from the library, and out of a nested function with saved registers),
 * RTTI (typeid, dynamic_cast), virtual functions, new and delete, and the
 * standard library: string, vector, map, unique_ptr, shared_ptr, sort,
 * function, iostream and stringstream. tests/run.sh runs the 68k build in
 * tools/sim68k.py --os and the PowerPC build in tools/simppc.py, and
 * checks the console output that each leaves: one "ok" line per check,
 * then "all ok".
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

static int failures;

/// Print one result line and count the failures.
static void check(const char *what, bool ok) {
  std::printf("%s %s\n", ok ? "ok" : "FAILED", what);
  if (!ok)
    failures++;
}

/// A global object whose constructor runs before main.
struct Global {
  int value;
  /// Set the value, to show that the constructor ran.
  Global() : value(42) {}
};
static Global global;

static int destroyed;

/// Counts its destructions, to show that unwinding runs destructors.
struct Guard {
  /// Count one destruction.
  ~Guard() { destroyed++; }
};

/// The base class of the exceptions that the test throws.
struct Base {
  virtual ~Base() {}
  /// Name the class, through the virtual table.
  virtual const char *name() const { return "Base"; }
};

/// A class derived from Base, for catch by base class and dynamic_cast.
struct Derived : Base {
  int code;
  /// Keep the code to check it where the exception is caught.
  explicit Derived(int c) : code(c) {}
  const char *name() const override { return "Derived"; }
};

/// Throw a Derived from below two frames that have destructors to run.
__attribute__((noinline)) static void deep(int depth, int code) {
  Guard g;
  if (depth == 0)
    throw Derived(code);
  deep(depth - 1, code);
}

/// Keep values in callee-saved registers across a throwing call, so that
/// the unwinder must restore them for the handler.
__attribute__((noinline)) static int keep_registers(int a, int b) {
  volatile int x = a * 3, y = b * 5;
  int s1 = x + 1, s2 = y + 2, s3 = x * y, s4 = x - y;
  try {
    deep(2, s1);
  } catch (const Derived &d) {
    return s1 + s2 + s3 + s4 + d.code;
  }
  return -1;
}

/// Throw an int, to catch it with catch (...) and rethrow it.
__attribute__((noinline)) static void throw_int() { throw 7; }

int main() {
  check("static constructor", global.value == 42);

  destroyed = 0;
  try {
    deep(3, 99);
    check("throw", false);
  } catch (Derived &d) {
    check("catch by type", d.code == 99);
    check("destructors while unwinding", destroyed == 4);
  }

  try {
    deep(0, 5);
  } catch (const Base &b) {
    check("catch by base class", std::strcmp(b.name(), "Derived") == 0);
  }

  {
    int a = 2, b = 3;   /* x = 6, y = 15: 7 + 17 + 90 - 9 + 7 */
    check("registers restored", keep_registers(a, b) == 112);
  }

  int caught = 0;
  try {
    try {
      throw_int();
    } catch (...) {
      caught = 1;
      throw;
    }
  } catch (int v) {
    check("catch (...) and rethrow", caught == 1 && v == 7);
  }

  try {
    std::vector<int> v(3);
    v.at(10) = 1;
    check("std::out_of_range", false);
  } catch (const std::out_of_range &) {
    check("std::out_of_range", true);
  }

  try {
    throw std::runtime_error("message");
  } catch (const std::exception &e) {
    check("std::runtime_error", std::strcmp(e.what(), "message") == 0);
  }

  {
    Base *p = new Derived(3);
    check("typeid", typeid(*p) == typeid(Derived) &&
                        std::strcmp(typeid(int).name(), "i") == 0);
    Derived *d = dynamic_cast<Derived *>(p);
    check("dynamic_cast", d != nullptr && d->code == 3);
    Base other;
    check("dynamic_cast fails", dynamic_cast<Derived *>(&other) == nullptr);
    check("virtual call", std::strcmp(p->name(), "Derived") == 0);
    delete p;
  }

  {
    std::string s = "Hello";
    s += ", Mac";
    check("std::string", s == "Hello, Mac" && s.size() == 10 &&
                             s.find("Mac") == 7);
    std::vector<int> v = {5, 3, 9, 1};
    std::sort(v.begin(), v.end());
    check("std::vector and std::sort",
          v.size() == 4 && v[0] == 1 && v[3] == 9);
    std::map<std::string, int> m;
    m["one"] = 1;
    m["two"] = 2;
    check("std::map", m.size() == 2 && m["two"] == 2);
    std::unique_ptr<Base> u(new Derived(4));
    std::shared_ptr<int> sp = std::make_shared<int>(8);
    std::shared_ptr<int> sp2 = sp;
    check("smart pointers", u->name()[0] == 'D' && *sp2 == 8 &&
                                sp.use_count() == 2);
    std::function<int(int)> f = [&](int x) { return x + v[3]; };
    check("std::function", f(1) == 10);
  }

  {
    std::ostringstream os;
    os << "n=" << 12 << " f=" << 2.5;
    check("std::ostringstream", os.str() == "n=12 f=2.5");
    std::istringstream is("34 56");
    int x = 0, y = 0;
    is >> x >> y;
    check("std::istringstream", x == 34 && y == 56);
  }

  std::cout << "std::cout works" << std::endl;
  std::printf(failures ? "FAILED\n" : "all ok\n");
  return 0;
}
