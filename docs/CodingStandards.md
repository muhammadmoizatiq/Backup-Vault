# BackupVault — Coding Standards

**Applies to:** Qasim, Abdul Rehman and Moiz  
**Language:** C++17  
**Framework:** Qt 6

## 1. Main rule: keep coding simple

Always choose the simplest implementation that correctly satisfies the project requirements.

The code should be easy for a third-semester student to:
- read;
- understand;
- explain to teammates and an instructor;
- test;
- debug;
- maintain.

Do not make code complicated just to make it look advanced.

## 2. Prefer straightforward code

- Use clear variable, class and function names.
- Keep functions focused and reasonably short.
- Prefer simple `if`, `else`, and loop logic.
- Avoid deeply nested conditions when a simple early return is clearer.
- Avoid unnecessary helper functions, classes, templates, inheritance, and design patterns.
- Do not add a new dependency unless the project genuinely needs it and the team agrees.
- Do not duplicate logic, but do not create a complicated abstraction just to remove a tiny repetition.

## 3. Follow the assignment's DSA requirements

The project is a Data Structures and Algorithms assignment. Required data structures must be implemented manually in their assigned modules.

Examples:
- Stack and Queue in `FileScanner`.
- N-ary Tree in `FileNode`.
- Hash Table with Linked List chaining in `ChunkIndex`.
- Min Heap and Huffman Tree in `Huffman`.

Do not replace a required manual implementation with a library structure. For example, do not use `std::priority_queue` for the Huffman Min Heap.

Use standard library containers where they are not replacing a required DSA implementation and where the team interface allows them.

## 4. Memory and pointers

- Prefer automatic/local objects and standard value types when practical.
- Do not use raw pointers or manual `new`/`delete` unless they are genuinely required by the data structure or Qt interface.
- When dynamic allocation is required, make ownership and deletion clear.
- Do not introduce smart-pointer patterns or ownership wrappers without understanding why they are needed.
- Never leave dangling pointers, memory leaks, or unclear ownership.

## 5. Shared project conventions

Follow the architecture's shared conventions:

- Namespace: `bv`.
- Byte data: `bv::Bytes` (defined in the shared common header).
- Paths: `QString`.
- Hashing: SHA-256 using `QCryptographicHash`.
- Public module errors: `Result<T>` or `Status`.
- No exceptions across module boundaries.
- SQLite access through Qt SQL (`QSQLITE`).

Do not independently invent a second version of a shared type.

## 6. Interfaces and teamwork

- Keep each member's work inside the assigned module.
- Do not change another member's public interface without discussing it first.
- Changes to `include/common/` require review from all three members.
- Explain interface changes before implementing them.
- Keep shared headers small and focused.
- Use the agreed architecture and database schema as the reference.

## 7. Error handling and data safety

- Check file, database, compression and decompression operations for failure.
- Return errors through `Result<T>` or `Status` at public module boundaries.
- Use prepared SQL statements.
- Use transactions for related database changes.
- Treat chunk contents as arbitrary binary data, not text.
- Never delete a chunk while its committed reference count is above zero.
- Do not silently ignore corrupt, missing or truncated data.
- Do not claim an operation succeeded unless it was verified.

## 8. Comments and explanations

- Comment why non-obvious logic is needed, not every obvious line.
- Keep comments accurate when code changes.
- Before a substantial implementation, explain what will be changed and why.
- After a meaningful change, explain:
  1. What changed.
  2. Why it was needed.
  3. How it works.
  4. How to test it.

## 9. Make small, testable changes

- Implement one small part at a time.
- Build after meaningful changes.
- Test normal cases and important edge cases.
- Do not rewrite working code without a clear reason.
- Do not mix unrelated changes into one commit.
- Never fabricate build or test results.

## 10. Before adding complexity, ask

Before adding a class, helper, dependency, abstraction, or advanced technique, ask:

1. Is it required by the project?
2. Does it make the implementation easier to understand?
3. Can the same requirement be met with simpler code?
4. Can I explain it to my teammates and instructor?

If the extra complexity is not justified, do not add it.

## 11. Final principle

**Simple to read. Simple to explain. Simple to test. Safe and correct in operation.**

Simplicity never means removing required functionality, weakening data integrity, skipping error handling, or ignoring the project's DSA requirements.
