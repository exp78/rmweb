// Host unit test for the pure link-hint label generator (no Qt, no device). Build+run on the host:
//   clang++ -std=c++17 -o build/hintlabels_test tests/hintlabels_test.cpp && ./build/hintlabels_test
#include "../engine/wpeqt/hintlabels.h"
#include <cassert>
#include <cstdio>
#include <set>
using namespace rmweb;
int main() {
    assert(hintLabels(0).empty());                                   // nothing to hint
    { auto v = hintLabels(1); assert(v.size()==1 && v[0]=="a"); }     // single char
    { auto v = hintLabels(3); assert(v.size()==3 && v[0]=="a" && v[1]=="s" && v[2]=="d"); }
    { auto v = hintLabels(9); assert(v.size()==9); for (auto&s:v) assert(s.size()==1); } // fits in 1 char
    { auto v = hintLabels(10); assert(v.size()==10); for (auto&s:v) assert(s.size()==2);  // needs 2 chars
      std::set<std::string> uniq(v.begin(), v.end()); assert(uniq.size()==10); }          // all unique
    { HintConfig c; c.minLen=2; auto v=hintLabels(2,c); for (auto&s:v) assert(s.size()==2); } // length floor
    { auto v = hintLabels(100); std::set<std::string> u(v.begin(),v.end());
      assert(v.size()==100 && u.size()==100); }                                          // 100 unique
    { HintConfig c; c.chars=""; assert(hintLabels(5,c).empty()); }    // empty-alphabet guard
    printf("hintlabels tests OK\n");
    return 0;
}
