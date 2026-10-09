#pragma once
// SneekPeek - tiny arithmetic evaluator. Header-only, no Win32 dependency,
// so the unit tests compile and exercise it directly.
//
// Grammar (precedence high to low): primary, power (^, right-associative),
// unary (+/-), product (* / %), sum (+ -). So -2^2 == -(2^2) == -4.
// Division/modulo by zero and non-finite results are errors, not silent 0.
#include <cwctype>
#include <cmath>
#include <string>

struct CalcParser {
    static const int kMaxDepth = 64;
    const wchar_t* p;
    bool ok = true;
    int depth = 0;
    void Skip() { while (*p && iswspace(*p)) p++; }
    double ParseExpr() {
        double v = ParseTerm();
        for (;;) { Skip(); if (*p==L'+'){p++; v+=ParseTerm();} else if(*p==L'-'){p++; v-=ParseTerm();} else break; }
        return v;
    }
    double ParseTerm() {
        double v = ParseUnary();
        for (;;) {
            Skip();
            if (*p==L'*') { p++; v *= ParseUnary(); }
            else if (*p==L'/') { p++; double d = ParseUnary(); if (d == 0) { ok = false; return 0; } v /= d; }
            else if (*p==L'%') { p++; double d = ParseUnary(); if (d == 0) { ok = false; return 0; } v = fmod(v, d); }
            else break;
        }
        return v;
    }
    double ParseUnary() {
        Skip();
        if (*p==L'-'){ p++; return -ParseUnary(); }
        if (*p==L'+'){ p++; return ParseUnary(); }
        return ParsePower();
    }
    double ParsePower() {
        double base = ParsePrimary();
        Skip();
        if (*p==L'^'){ p++; double e = ParseUnary(); base = pow(base, e); }
        return base;
    }
    double ParsePrimary() {
        Skip();
        if (++depth > kMaxDepth) { ok = false; return 0; }
        double r;
        if (*p==L'('){ p++; double v=ParseExpr(); Skip(); if(*p==L')') p++; else ok=false; r = v; }
        else if ((*p>=L'0'&&*p<=L'9')||*p==L'.'){
            wchar_t* end=nullptr; double v=wcstod(p,&end);
            if(end==p){ok=false; r=0;} else { p=end; r=v; }
        }
        else { ok=false; r=0; }
        depth--;
        return r;
    }
};

inline bool TryCalc(const std::wstring& q, double& out) {
    if (q.empty() || q.size() > 256) return false; // pasted-garbage guard
    bool hasDigit=false, hasOp=false;
    for (wchar_t c:q){
        if(c>=L'0'&&c<=L'9') hasDigit=true;
        else if(c==L'+'||c==L'-'||c==L'*'||c==L'/'||c==L'%'||c==L'^'||c==L'('||c==L')'||c==L'.'||c==L' '||c==L'\t') { if(c!=L' '&&c!=L'\t'&&c!=L'.') hasOp=true; }
        else return false;
    }
    if(!hasDigit||!hasOp) return false;
    CalcParser cp{ q.c_str() };
    double v = cp.ParseExpr();
    cp.Skip();
    if(!cp.ok || *cp.p!=L'\0' || !std::isfinite(v)) return false;
    out=v; return true;
}
