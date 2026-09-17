// Standalone harness for the [Sleeper] bounded first-fit walk. It mirrors the
// control flow of HadesGC::OldGen::search exactly -- the bucket loop, the
// per-bucket segment list, the per-segment cell list, and the two hit tests --
// with the VM types replaced by plain structs, so the break logic can be
// exercised without a configured Hermes build.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

static constexpr uint32_t kSleeperMaxSearchCellsPerBucket = 64;

// Mirrors HadesGC::OldGen::Budgeted.
enum class Budgeted { No, Yes };
static constexpr size_t kNumFreelistBuckets = 64;
static constexpr uint32_t kMinAllocationSize = 32;

struct Cell {
  uint32_t size;
  Cell *next;
};
struct SegBucket {
  Cell *head;
  SegBucket *next;
};

// Instrumentation, matching gc_.ygFreelistCellsWalked_.
static uint64_t g_cellsWalked = 0;

struct Heap {
  // buckets[b] is the head of the segment list for bucket b.
  std::vector<SegBucket *> buckets{kNumFreelistBuckets, nullptr};

  size_t findNextSetBitFrom(size_t from) const {
    for (size_t i = from; i < kNumFreelistBuckets; i++)
      if (buckets[i])
        return i;
    return kNumFreelistBuckets;
  }

  Cell *search(uint32_t sz, size_t startBucket, Budgeted budgeted) {
    const uint32_t cellBudget =
        budgeted == Budgeted::Yes ? kSleeperMaxSearchCellsPerBucket : ~0u;
    size_t bucket = findNextSetBitFrom(startBucket);
    for (; bucket < kNumFreelistBuckets;
         bucket = findNextSetBitFrom(bucket + 1)) {
      uint32_t cellsWalkedInBucket = 0;
      bool budgetExhausted = false;
      auto *segBucket = buckets[bucket];
      do {
        Cell *cell = segBucket->head;
        do {
          if (cellsWalkedInBucket >= cellBudget) {
            budgetExhausted = true;
            break;
          }
          ++cellsWalkedInBucket;
          ++g_cellsWalked;
          const auto cellSize = cell->size;
          if (cellSize >= sz + kMinAllocationSize)
            return cell; // split path
          else if (cellSize == sz)
            return cell; // exact path
          cell = cell->next;
        } while (cell);
        if (budgetExhausted)
          break;
        segBucket = segBucket->next;
      } while (segBucket);
    }
    return nullptr;
  }
};

// Build a chain of `n` cells all of `size`, optionally ending in one `fitSize`.
static Cell *chain(uint32_t size, int n, int fitAt = -1, uint32_t fitSize = 0) {
  Cell *head = nullptr;
  for (int i = n - 1; i >= 0; i--)
    head = new Cell{i == fitAt ? fitSize : size, head};
  return head;
}

static int failures = 0;
static void check(bool ok, const char *what) {
  printf("  %-72s %s\n", what, ok ? "pass" : "FAIL");
  if (!ok)
    failures++;
}

int main() {
  const uint32_t sz = 512;

  printf("1. a reachable fit inside the budget is still found\n");
  {
    Heap h;
    SegBucket sb{chain(256, 10, 5, 4096), nullptr};
    h.buckets[10] = &sb;
    g_cellsWalked = 0;
    Cell *r = h.search(sz, 10, Budgeted::Yes);
    check(r && r->size == 4096, "returns the fitting cell");
    check(g_cellsWalked == 6, "walks only up to it (6 cells)");
  }

  printf("2. an exact match is taken\n");
  {
    Heap h;
    SegBucket sb{chain(256, 4, 2, sz), nullptr};
    h.buckets[10] = &sb;
    g_cellsWalked = 0;
    Cell *r = h.search(sz, 10, Budgeted::Yes);
    check(r && r->size == sz, "returns the exact-size cell");
  }

  printf("3. THE FIX: a hopeless bucket is abandoned at the budget\n");
  {
    Heap h;
    // 100k cells too small to ever satisfy sz, the captured pathology.
    SegBucket sb{chain(256, 100000), nullptr};
    h.buckets[10] = &sb;
    g_cellsWalked = 0;
    Cell *r = h.search(sz, 10, Budgeted::Yes);
    check(r == nullptr, "gives up rather than finding a fit");
    check(
        g_cellsWalked == kSleeperMaxSearchCellsPerBucket,
        "walks exactly the budget, not 100,000");
  }

  printf("4. abandoning a bucket still leaves larger buckets reachable\n");
  {
    Heap h;
    SegBucket hopeless{chain(256, 100000), nullptr};
    SegBucket bigger{chain(8192, 3), nullptr};
    h.buckets[10] = &hopeless;
    h.buckets[11] = &bigger;
    g_cellsWalked = 0;
    Cell *r = h.search(sz, 10, Budgeted::Yes);
    check(r && r->size == 8192, "finds the fit in the next bucket up");
    check(
        g_cellsWalked == kSleeperMaxSearchCellsPerBucket + 1,
        "budget on the hopeless bucket, then one cell in the next");
  }

  printf("5. the budget spans a bucket's segments, not each one separately\n");
  {
    Heap h;
    // Three segments, each with more cells than the budget.
    SegBucket s3{chain(256, 200), nullptr};
    SegBucket s2{chain(256, 200), &s3};
    SegBucket s1{chain(256, 200), &s2};
    h.buckets[10] = &s1;
    g_cellsWalked = 0;
    h.search(sz, 10, Budgeted::Yes);
    check(
        g_cellsWalked == kSleeperMaxSearchCellsPerBucket,
        "one budget total, not one per segment");
  }

  printf("6. a fit in a later segment of the same bucket, within budget\n");
  {
    Heap h;
    SegBucket s2{chain(4096, 1), nullptr};
    SegBucket s1{chain(256, 10), &s2};
    h.buckets[10] = &s1;
    g_cellsWalked = 0;
    Cell *r = h.search(sz, 10, Budgeted::Yes);
    check(r && r->size == 4096, "crosses into the next segment and finds it");
  }

  printf("7. an empty heap still returns nullptr\n");
  {
    Heap h;
    g_cellsWalked = 0;
    check(h.search(sz, 0, Budgeted::Yes) == nullptr, "no buckets set -> nullptr");
    check(g_cellsWalked == 0, "walks nothing");
  }

  printf("8. THE OOM FIX: Budgeted::No finds a fit past the budget\n");
  {
    Heap h;
    // The cell that fits sits at position 5000, far beyond the budget. This is
    // the state allocSlow is in after waitForCollectionToFinish: if search
    // returns nullptr here, the next stop is oom().
    SegBucket sb{chain(256, 100000, 5000, 4096), nullptr};
    h.buckets[10] = &sb;

    g_cellsWalked = 0;
    check(
        h.search(sz, 10, Budgeted::Yes) == nullptr,
        "budgeted search misses it -- this is what would have crashed");

    g_cellsWalked = 0;
    Cell *r = h.search(sz, 10, Budgeted::No);
    check(r && r->size == 4096, "unbudgeted search finds it, so no spurious OOM");
    check(g_cellsWalked == 5001, "and walks the whole way to it");
  }

  printf("9. Budgeted::No on a genuinely full heap still reports failure\n");
  {
    Heap h;
    SegBucket sb{chain(256, 1000), nullptr};
    h.buckets[10] = &sb;
    g_cellsWalked = 0;
    check(h.search(sz, 10, Budgeted::No) == nullptr, "nullptr when nothing fits");
    check(g_cellsWalked == 1000, "having actually checked every cell");
  }

  printf("\n%s\n", failures ? "FAILURES PRESENT" : "all checks passed");
  return failures ? 1 : 0;
}
