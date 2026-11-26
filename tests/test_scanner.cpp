#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <iostream>

#include "core/FileScanner.h"

using namespace bv;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (!condition) {
        std::cout << "FAIL: " << what << std::endl;
        ++failures;
    }
}

static void makeFile(const QString &path)
{
    QFile f(path);
    bool opened = f.open(QIODevice::WriteOnly);
    check(opened, "test helper can create a file");
    f.write("x");
}

// Turn absolute paths into paths relative to root ("" for root itself),
// so the expected lists do not depend on where the temp folder is.
static QStringList relative(const std::vector<QString> &paths, const QString &root)
{
    QStringList result;
    for (const QString &p : paths)
        result << p.mid(root.size());
    return result;
}

static void showIfDifferent(const QStringList &actual, const QStringList &expected, const char *what)
{
    bool same = (actual == expected);
    check(same, what);
    if (!same)
        std::cout << "  actual:   " << qPrintable(actual.join(", ")) << std::endl;
}

int main()
{
    // ---- Build a small folder tree ----
    //   root/
    //     a_dir/
    //       deep/
    //         deep.txt
    //       inner.txt
    //     b_empty/
    //     readme.txt
    QTemporaryDir tmp;
    check(tmp.isValid(), "temp dir created");
    const QString base = tmp.path();
    QDir().mkpath(base + "/a_dir/deep");
    QDir().mkpath(base + "/b_empty");
    makeFile(base + "/a_dir/inner.txt");
    makeFile(base + "/a_dir/deep/deep.txt");
    makeFile(base + "/readme.txt");

    FileScanner scanner;
    auto scanned = scanner.scan(base);
    check(scanned.isOk(), "scan of a valid folder succeeds");

    if (scanned.isOk()) {
        const FileNode &root = scanned.value();
        const QString rootPath = root.path();

        // ---- Tree shape ----
        check(root.isDirectory(), "root is a directory");
        check(root.children().size() == 3, "root has 3 children");
        if (root.children().size() == 3) {
            check(root.children()[0].name() == "a_dir" && root.children()[0].isDirectory(),
                  "first child is folder a_dir");
            check(root.children()[1].name() == "b_empty" && root.children()[1].isDirectory(),
                  "second child is folder b_empty");
            check(root.children()[1].children().empty(), "empty folder is kept with no children");
            check(root.children()[2].name() == "readme.txt" && !root.children()[2].isDirectory(),
                  "third child is file readme.txt");
            check(root.children()[2].children().empty(), "a file has no children");
            check(root.children()[0].children().size() == 2, "a_dir has 2 children");
        }
        check(scanner.warnings().empty(), "no warnings for a normal folder");

        // ---- Traversal order ----
        showIfDifferent(relative(scanner.traverseDFS(root), rootPath),
                        QStringList{"", "/a_dir", "/a_dir/deep", "/a_dir/deep/deep.txt",
                                    "/a_dir/inner.txt", "/b_empty", "/readme.txt"},
                        "DFS order");
        showIfDifferent(relative(scanner.traverseBFS(root), rootPath),
                        QStringList{"", "/a_dir", "/b_empty", "/readme.txt", "/a_dir/deep",
                                    "/a_dir/inner.txt", "/a_dir/deep/deep.txt"},
                        "BFS order");
    }

    // ---- Errors ----
    auto missing = scanner.scan(base + "/does_not_exist");
    check(!missing.isOk(), "missing folder fails");
    check(!missing.message().isEmpty(), "missing folder has an error message");

    auto notFolder = scanner.scan(base + "/readme.txt");
    check(!notFolder.isOk(), "a file path fails (not a folder)");

    // ---- Empty root folder ----
    QTemporaryDir emptyDir;
    auto emptyScan = scanner.scan(emptyDir.path());
    check(emptyScan.isOk(), "scan of an empty folder succeeds");
    if (emptyScan.isOk()) {
        check(emptyScan.value().children().empty(), "empty root has no children");
        check(scanner.traverseDFS(emptyScan.value()).size() == 1, "DFS of empty root gives 1 path");
        check(scanner.traverseBFS(emptyScan.value()).size() == 1, "BFS of empty root gives 1 path");
    }

    // ---- Symbolic link is skipped with a warning ----
    QTemporaryDir linkDir;
    const QString realFile = linkDir.path() + "/real.txt";
    makeFile(realFile);
    QString linkPath = linkDir.path() + "/mylink";
#ifdef Q_OS_WIN
    linkPath += ".lnk";  // on Windows Qt creates a shortcut file
#endif
    if (QFile::link(realFile, linkPath)) {
        auto linkScan = scanner.scan(linkDir.path());
        check(linkScan.isOk(), "scan with a link succeeds");
        if (linkScan.isOk())
            check(linkScan.value().children().size() == 1, "link is not added to the tree");
        check(scanner.warnings().size() == 1, "one warning for the skipped link");
    } else {
        std::cout << "SKIP: could not create a link on this system" << std::endl;
    }

    // ---- Warnings are cleared by the next scan ----
    scanner.scan(base);
    check(scanner.warnings().empty(), "warnings reset on a new scan");

    // ---- Manual Stack is last in, first out ----
    FileNode n1("one", "/one", false);
    FileNode n2("two", "/two", false);
    FileNode n3("three", "/three", false);
    {
        NodeStack stack;
        check(stack.isEmpty(), "new stack is empty");
        stack.push(&n1);
        stack.push(&n2);
        stack.push(&n3);
        check(!stack.isEmpty(), "stack not empty after push");
        check(stack.pop() == &n3, "stack pops last pushed first");
        check(stack.pop() == &n2, "stack pops second");
        check(stack.pop() == &n1, "stack pops first");
        check(stack.isEmpty(), "stack empty after popping all");
    }

    // ---- Manual Queue is first in, first out ----
    {
        NodeQueue queue;
        check(queue.isEmpty(), "new queue is empty");
        queue.enqueue(&n1);
        queue.enqueue(&n2);
        queue.enqueue(&n3);
        check(queue.dequeue() == &n1, "queue returns first added first");
        check(queue.dequeue() == &n2, "queue returns second");
        queue.enqueue(&n1);  // add again after some removals
        check(queue.dequeue() == &n3, "queue returns third");
        check(queue.dequeue() == &n1, "queue returns the re-added node");
        check(queue.isEmpty(), "queue empty after removing all");
    }

    if (failures == 0)
        std::cout << "All scanner tests passed." << std::endl;
    return failures == 0 ? 0 : 1;
}
