#include <iostream>

#include "compression/HuffmanTree.h"

using namespace bv;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (!condition) {
        std::cout << "FAIL: " << what << std::endl;
        ++failures;
    }
}

using Frequencies = std::array<std::uint64_t, kHuffmanSymbolCount>;

int main()
{
    // ---- Building from frequencies ------------------------------------
    // Classic textbook example (CLRS): a:5 b:9 c:12 d:13 e:16 f:45.
    // Expected depths: f=1, c=d=e=3, a=b=4.
    {
        Frequencies frequency{};
        frequency['a'] = 5;
        frequency['b'] = 9;
        frequency['c'] = 12;
        frequency['d'] = 13;
        frequency['e'] = 16;
        frequency['f'] = 45;

        HuffmanTree tree;
        tree.buildFromFrequencies(frequency);
        HuffmanLengths lengths;
        check(tree.codeLengths(lengths, kHuffmanMaxCodeLength), "textbook tree builds");
        check(lengths['f'] == 1, "f has a 1-bit code");
        check(lengths['c'] == 3 && lengths['d'] == 3 && lengths['e'] == 3,
              "c, d, e have 3-bit codes");
        check(lengths['a'] == 4 && lengths['b'] == 4, "a, b have 4-bit codes");
        check(lengths['z'] == 0, "unused symbol has length 0");

        // Walking: the root is not a leaf and has two children.
        check(!tree.isLeaf(tree.root()), "root of a multi-symbol tree is an inner node");
        check(tree.child(tree.root(), false) != kNoNode
                  && tree.child(tree.root(), true) != kNoNode,
              "root has both children");
    }

    // Empty and single-symbol trees.
    {
        HuffmanTree tree;
        tree.buildFromFrequencies(Frequencies{});
        HuffmanLengths lengths;
        lengths.fill(9);
        check(tree.codeLengths(lengths, kHuffmanMaxCodeLength), "empty tree ok");
        bool allZero = true;
        for (std::uint8_t length : lengths)
            allZero = allZero && length == 0;
        check(allZero, "empty tree: all lengths 0");

        Frequencies one{};
        one[42] = 1000;
        tree.buildFromFrequencies(one);
        check(tree.codeLengths(lengths, kHuffmanMaxCodeLength), "single-symbol tree ok");
        check(lengths[42] == 1, "single symbol gets a 1-bit code");
        check(tree.isLeaf(tree.root()) && tree.symbolAt(tree.root()) == 42,
              "single-symbol root is the leaf");
    }

    // Two symbols both get 1 bit.
    {
        Frequencies two{};
        two[0] = 1;
        two[255] = 100;
        HuffmanTree tree;
        tree.buildFromFrequencies(two);
        HuffmanLengths lengths;
        check(tree.codeLengths(lengths, kHuffmanMaxCodeLength), "two-symbol tree ok");
        check(lengths[0] == 1 && lengths[255] == 1, "two symbols: 1 bit each");
    }

    // The same frequencies always give the same lengths.
    {
        Frequencies frequency{};
        for (int i = 0; i < 256; ++i)
            frequency[i] = static_cast<std::uint64_t>((i * 37) % 11 + 1);
        HuffmanTree first;
        HuffmanTree second;
        first.buildFromFrequencies(frequency);
        second.buildFromFrequencies(frequency);
        HuffmanLengths a;
        HuffmanLengths b;
        check(first.codeLengths(a, 32) && second.codeLengths(b, 32) && a == b,
              "building twice gives the same lengths");
    }

    // A code longer than the limit is reported, not truncated.
    // Frequencies 1, 1, 2, 4, 8, 16 give a chain of depth 5.
    {
        Frequencies chain{};
        std::uint64_t value = 1;
        chain[0] = 1;
        for (int i = 1; i <= 5; ++i) {
            chain[i] = value;
            value *= 2;
        }
        HuffmanTree tree;
        tree.buildFromFrequencies(chain);
        HuffmanLengths lengths;
        check(tree.codeLengths(lengths, 5), "depth 5 fits in a limit of 5");
        check(lengths[0] == 5 && lengths[1] == 5, "rarest symbols have 5-bit codes");
        check(!tree.codeLengths(lengths, 4), "depth 5 does not fit in a limit of 4");
    }

    // ---- Building from codes (decoder) ---------------------------------
    // a = 0, b = 10, c = 11
    {
        HuffmanTree tree;
        check(tree.addCode('a', 0, 1), "add a = 0");
        check(tree.addCode('b', 2, 2), "add b = 10");
        check(tree.addCode('c', 3, 2), "add c = 11");

        int node = tree.child(tree.root(), false);
        check(tree.isLeaf(node) && tree.symbolAt(node) == 'a', "bit 0 reaches a");

        node = tree.child(tree.root(), true);
        check(node != kNoNode && !tree.isLeaf(node), "bit 1 reaches an inner node");
        check(tree.symbolAt(tree.child(node, false)) == 'b', "bits 10 reach b");
        check(tree.symbolAt(tree.child(node, true)) == 'c', "bits 11 reach c");
        check(tree.child(tree.child(tree.root(), false), false) == kNoNode,
              "a leaf has no children");
    }

    // Bad codes are rejected.
    {
        HuffmanTree tree;
        check(tree.addCode('a', 0, 1), "first code accepted");
        check(!tree.addCode('x', 0, 1), "same code twice rejected");
        check(!tree.addCode('y', 0, 2), "code that starts with an existing code rejected");

        HuffmanTree other;
        check(other.addCode('a', 0, 2), "a = 00 accepted");
        check(!other.addCode('b', 0, 1), "code that is the start of an existing code rejected");
        check(!other.addCode('c', 1, 0), "length 0 rejected");
        check(!other.addCode('d', 0, 33), "length above 32 rejected");
    }

    // clear() makes the tree reusable.
    {
        HuffmanTree tree;
        tree.addCode('a', 0, 1);
        tree.clear();
        check(tree.addCode('b', 0, 1), "same code accepted again after clear");
        check(tree.symbolAt(tree.child(tree.root(), false)) == 'b', "new tree has the new symbol");
    }

    // Walking with invalid indexes is safe.
    {
        HuffmanTree tree;
        check(tree.child(0, false) == kNoNode, "child of node in empty tree");
        check(tree.child(-1, true) == kNoNode, "child of negative node");
        check(!tree.isLeaf(5), "isLeaf of unknown node");
        check(tree.symbolAt(5) == kNotASymbol, "symbolAt of unknown node");
    }

    if (failures == 0)
        std::cout << "All HuffmanTree checks passed." << std::endl;

    return failures == 0 ? 0 : 1;
}
