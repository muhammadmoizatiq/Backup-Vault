#pragma once

#include <QString>
#include <utility>

namespace bv {

// Success or an error message. Public module functions return this
// instead of throwing exceptions.
class Status {
public:
    static Status success() { return Status(true, QString()); }
    static Status failure(const QString &message) { return Status(false, message); }

    bool isOk() const { return m_ok; }
    const QString &message() const { return m_message; }

private:
    Status(bool ok, const QString &message) : m_ok(ok), m_message(message) {}
    bool m_ok;
    QString m_message;
};

// Either a value or an error message.
template <typename T>
class Result {
public:
    static Result success(T value) { return Result(true, std::move(value), QString()); }
    static Result failure(const QString &message) { return Result(false, T(), message); }

    bool isOk() const { return m_ok; }
    const T &value() const { return m_value; }
    const QString &message() const { return m_message; }
    Status status() const { return m_ok ? Status::success() : Status::failure(m_message); }

private:
    Result(bool ok, T value, const QString &message)
        : m_ok(ok), m_value(std::move(value)), m_message(message) {}
    bool m_ok;
    T m_value;
    QString m_message;
};

}  // namespace bv
