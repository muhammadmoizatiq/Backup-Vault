#include <iostream>
#include <vector>

#include "compression/MinHeap.h"

using namespace bv;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (!condition) {
        std::cout << "FAIL: " << what << std::endl;
        ++failures;
    }
}

static std::vector<HuffmanNode> makeNodes(const std::vector<std::uint64_t> &frequencies)
{
    std::vector<HuffmanNode> nodes;
    for (std::uint64_t frequency : frequencies)
        nodes.push_back({frequency, kNoNode, kNoNode, 0});
    return nodes;
}

int main()
{
    // Empty heap.
    {
        const std::vector<HuffmanNode> nodes = makeNodes({});
        MinHeap heap(nodes);
        check(heap.isEmpty(), "new heap is empty");
        check(heap.size() == 0, "new heap has size 0");
        check(heap.pop() == kNoNode, "pop on empty heap returns kNoNode");
    }

    // Items come out smallest frequency first.
    {
        const std::vector<HuffmanNode> nodes = makeNodes({50, 10, 40, 20, 30, 5, 60});
        MinHeap heap(nodes);
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
            heap.push(i);
        check(heap.size() == nodes.size(), "size after pushes");

        std::uint64_t previous = 0;
        bool sorted = true;
        std::size_t popped = 0;
        while (!heap.isEmpty()) {
            const int index = heap.pop();
            if (index == kNoNode || nodes[index].frequency < previous)
                sorted = false;
            else
                previous = nodes[index].frequency;
            ++popped;
        }
        check(sorted, "pops are in ascending frequency order");
        check(popped == nodes.size(), "every pushed item is popped once");
        check(heap.pop() == kNoNode, "heap is empty again after popping all");
    }

    // Equal frequencies: lower index first (makes the tree deterministic).
    {
        const std::vector<HuffmanNode> nodes = makeNodes({7, 7, 7, 7});
        MinHeap heap(nodes);
        heap.push(3);
        heap.push(1);
        heap.push(2);
        heap.push(0);
        check(heap.pop() == 0, "ties: index 0 first");
        check(heap.pop() == 1, "ties: index 1 second");
        check(heap.pop() == 2, "ties: index 2 third");
        check(heap.pop() == 3, "ties: index 3 last");
    }

    // Pushing between pops still returns the current smallest.
    {
        const std::vector<HuffmanNode> nodes = makeNodes({9, 3, 6, 1, 8});
        MinHeap heap(nodes);
        heap.push(0);  // 9
        heap.push(1);  // 3
        check(heap.pop() == 1, "smallest of {9, 3} is 3");
        heap.push(2);  // 6
        heap.push(3);  // 1
        check(heap.pop() == 3, "smallest of {9, 6, 1} is 1");
        heap.push(4);  // 8
        check(heap.pop() == 2, "smallest of {9, 6, 8} is 6");
        check(heap.pop() == 4, "then 8");
        check(heap.pop() == 0, "then 9");
        check(heap.isEmpty(), "empty at the end");
    }

    // Larger repeatable pseudo-random check (same on every platform).
    {
        std::vector<std::uint64_t> frequencies;
        std::uint32_t state = 12345;
        for (int i = 0; i < 2000; ++i) {
            state = state * 1664525u + 1013904223u;
            frequencies.push_back((state >> 16) % 500);
        }
        const std::vector<HuffmanNode> nodes = makeNodes(frequencies);
        MinHeap heap(nodes);
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
            heap.push(i);

        bool ordered = true;
        int previousIndex = kNoNode;
        while (!heap.isEmpty()) {
            const int index = heap.pop();
            if (previousIndex != kNoNode) {
                const HuffmanNode &a = nodes[previousIndex];
                const HuffmanNode &b = nodes[index];
                const bool inOrder = a.frequency < b.frequency
                                     || (a.frequency == b.frequency && previousIndex < index);
                if (!inOrder)
                    ordered = false;
            }
            previousIndex = index;
        }
        check(ordered, "2000 random items pop in (frequency, index) order");
    }

    if (failures == 0)
        std::cout << "All MinHeap checks passed." << std::endl;

    return failures == 0 ? 0 : 1;
}
