// flows debuggee (F6): a recursive visit() whose stepped line passes a callback into libSystem
// (qsort_b, which calls visit on the children) and then calls an inlined function.

#include <stdio.h>
#include <stdlib.h>

typedef struct Node Node;
struct Node { int value; int count; Node **children; };

volatile int g_tally = 0;

static inline int tally(int v)
{
  g_tally += v;                                        // @tally_body
  return g_tally;
}

int visit(Node *node)
{
  int sum = node->value;
  if(node->count > 0) { qsort_b(node->children, node->count, sizeof(Node *), ^int(const void *a, const void *b) { return visit(*(Node *const *)a) - visit(*(Node *const *)b); }); sum += tally(node->count); } // @visit_line
  return sum;                                          // @visit_after
}

int main(void)
{
  Node leaves[4] = {{3, 0, 0}, {1, 0, 0}, {4, 0, 0}, {2, 0, 0}};
  Node *kids_a[2] = {&leaves[0], &leaves[1]};
  Node *kids_b[2] = {&leaves[2], &leaves[3]};
  Node mids[2] = {{20, 2, kids_a}, {30, 2, kids_b}};
  Node *kids[2] = {&mids[0], &mids[1]};
  Node root = {10, 2, kids};
  int r = visit(&root);                                // @visit_main_call
  printf("visit %d\n", r);                             // @visit_main_print
  return r & 0x7f;
}
