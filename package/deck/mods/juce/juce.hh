/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * juce.hh - juce objects with a scope.
 *
 * juce::String, juce::Font and juce::StringArray are built by EP122's own
 * constructor and must be destroyed by its own destructor. Done by hand, an
 * early return between the two leaks into the app's heap.
 *
 * These classes hold the storage and tie the ctor/dtor pair to a scope. They
 * know only the object's size and the two entry points, not its layout.
 *
 *     juce::Font  f(14.0f);
 *     juce::String s("BROWSE");
 *     ep_call(void(void *, void *))::at(MOD_FN_GFX_SETFONT, g, f);
 *
 * Each converts to void * for a call that wants the object's address.
 */
#ifndef EP122_MOD_JUCE_HH
#define EP122_MOD_JUCE_HH

#include "juce/juce.h"
#include "juce/call.hh"

namespace juce {

/* juce::String is one pointer to shared ref-counted data. The buffer is larger
 * than that so a call cannot write past it. */
enum FromUtf8 { from_utf8 };

class String {
public:
    String() { ep_call(void(void *))::at(FN_STR_DEFCTOR, buf_); }

    /* FN_STR_CTOR is CharPointer_ASCII on this build: one byte becomes one
     * codepoint. Correct only for pure ASCII. */
    explicit String(const char *ascii)
    {
        ep_call(void(void *, const char *))::at(FN_STR_CTOR, buf_, ascii);
    }

    /* For text above U+007F. See juce_string_utf8. */
    String(const char *text, FromUtf8) { juce_string_utf8(buf_, text); }

    ~String() { ep_call(void(void *))::at(FN_STR_DTOR, buf_); }

    operator void *() { return buf_; }
    void *addr() { return buf_; }

private:
    String(const String &);
    String &operator=(const String &);

    alignas(16) uint8_t buf_[16];
};

/* The deck's own font at a given height. FN_FONT_BUILD returns it
 * indirectly; a 32-byte aggregate reproduces that, since AAPCS64 returns
 * anything above 16 bytes through x8. */
class Font {
public:
    explicit Font(float height)
    {
        store_ = ep_call(storage(float))::at(FN_FONT_BUILD, height);
    }

    ~Font() { ep_call(void(void *))::at(FN_FONT_DTOR, &store_); }

    operator void *() { return &store_; }
    void *addr() { return &store_; }

private:
    struct storage { alignas(16) uint8_t b[32]; };

    Font(const Font &);
    Font &operator=(const Font &);

    storage store_;
};

/* juce::StringArray. add() moves the string in; the String passed to it is
 * left empty and is still destroyed by its own scope. */
class StringArray {
public:
    StringArray() { ep_call(void(void *))::at(FN_STRARR_CTOR, buf_); }
    ~StringArray() { ep_call(void(void *))::at(FN_STRARR_DTOR, buf_); }

    void add(String &s)
    {
        ep_call(void(void *, void *))::at(FN_STRARR_ADD, buf_, s.addr());
    }

    void add(const char *utf8)
    {
        String s(utf8);
        add(s);
    }

    operator void *() { return buf_; }
    void *addr() { return buf_; }

private:
    StringArray(const StringArray &);
    StringArray &operator=(const StringArray &);

    alignas(16) uint8_t buf_[JUCE_STRARRAY_BYTES];
};

/* juce::var holding a String. FN_VAR_FROM_STR constructs it in place. */
class Var {
public:
    explicit Var(String &s)
    {
        ep_call(void(void *, void *))::at(FN_VAR_FROM_STR, buf_, s.addr());
    }

    ~Var() { ep_call(void(void *))::at(FN_VAR_DTOR, buf_); }

    operator void *() { return buf_; }
    void *addr() { return buf_; }

private:
    Var(const Var &);
    Var &operator=(const Var &);

    alignas(16) uint8_t buf_[16];
};

}  /* namespace juce */

#endif /* EP122_MOD_JUCE_HH */
