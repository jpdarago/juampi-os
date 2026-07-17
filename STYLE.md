Kernel Style Guide
==================

The following is a style guide for the kernel code. Code commits are evaluated
against it.

The rules are open to discussion except where noted, and there may be sections
of the current code that don't follow them. Let us know and we'll fix those
sections so they comply with the guide.

Style guide
-----------

0. 80 characters per line, 4 spaces per tab. This one is not up for discussion.
   Run `make format` (clang-format) before committing; `make lint` checks it and
   runs in CI.
1. Avoid assembly as much as you can. If it can be written in C, it MUST be
   written in C. Writing assembly causes serious problems. Acceptable exceptions
   must be thought through very carefully.
2. Use ANSI C99. In particular, study the bibliography at the end, because there
   are VERY important things to use, such as variadic macros, struct
   initializers, etc.
3. You may add warnings to CFLAGS. But you may NOT remove any. MUCH LESS remove
   `-Werror`. Ignoring warnings is asking for trouble. The current warnings are
   sufficient, and the compiler really is in strict mode.
4. If a pointer must not be modified, ALWAYS mark it `const`. If a value is
   polled by the code, ALWAYS mark it `volatile`.
5. Where possible, write code so that every function can be tested separately
   from the rest. Moreover, any module containing logic unrelated to hardware
   MUST be tested on its own (I'll define how to do this later).
6. Use long, descriptive names. Use named constants, not magic numbers. Write
   short functions (fitting in one editor screen is the limit; under 30 lines is
   desirable). Avoid long ifs or switches by using table-driven dispatching.
7. Comment the code. If you write code based on a source, cite the source (a
   link or a book page). Every non-obvious line should have its comment. This is
   NOT a substitute for writing clear code and using descriptive names.
8. If something MUST or MUST NOT happen, use a `fail_if` or a `fail_unless`.
   Failing as early as possible avoids a lot of trouble. Write tests using
   `fail_if` and `fail_unless` for code that has logic (it makes no sense for
   hardware-initialization code, but it does for a filesystem or a memory
   allocator). ASSUME NOTHING, as Mike Abrash says.
9. Every important data type must be declared in some `.h`.
10. Every function or symbol that should not be exported must be declared
    `static`.
11. DELEGATE AS MUCH AS YOU CAN TO THE COMPILER. TESTING A KERNEL IS VERY HARD.
12. All code and comments are in English.
13. Respect the licenses.

Sources
-------

* *21st Century C* by Ben Klemens
* *Learn C The Hard Way* by Zed Shaw (<http://c.learncodethehardway.org/book/>).
* <https://www.kernel.org/doc/Documentation/CodingStyle> — but ignore the
  tabs-are-8 part; 4 spaces here. Everything else is very reasonable. When in
  doubt about something not covered here, follow what it says.
* <https://github.com/mcinglis/c-style>, very good tips.
