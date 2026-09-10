#include "completed_frame_scope_v1.h"
#include <cstdio>
using S = a9tas::completed_frame_scope_v1::Stack<2,2>;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line=%d\n",__LINE__); return 1; } } while (0)
int main() {
  S s{}; S::Witness a{0x1234,7,10,90}, b{0x5678,8,11,91};
  CHECK(s.Current(10)==nullptr);
  CHECK(s.Push(10,a)); CHECK(s.Current(10)->tick==90);
  CHECK(s.Push(10,{})); CHECK(s.Current(10)==nullptr);
  CHECK(s.Push(10,a)); CHECK(s.Current(10)==nullptr); // depth overflow masks
  s.Pop(10); CHECK(s.Current(10)==nullptr);
  s.Pop(10); CHECK(s.Current(10)->generation==7);
  CHECK(s.Push(11,b)); CHECK(s.Current(11)->tick==91);
  CHECK(!s.Push(12,{0x1234,1,12,1}));
  s.Pop(12); CHECK(s.Current(10)->tick==90 && s.Current(11)->tick==91);
  s.Pop(10); CHECK(s.Current(10)==nullptr);
  CHECK(s.Push(12,{0x1234,9,12,2})); CHECK(s.Current(12)->generation==9);
  s.Pop(11); s.Pop(12); s.Pop(12);
  CHECK(!s.Push(0,{}));
  // A scope is a copied generation/tick snapshot, never a reference to mutable control.
  CHECK(s.Push(10,a)); a.generation=999; CHECK(s.Current(10)->generation==7);
  std::puts("FRAME_SCOPE passed=1 nested_unqualified overflow concurrent_tid reuse generation_snapshot");
}
