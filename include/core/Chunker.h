
#pragma once

#include <QString>
#include <vector>

#include "common/Types.h"
#include "common/Result.h"

namespace bv {

class Chunker {
public:
    static Result<std::vector<Bytes>> splitFile(
        const QString& filePath
    );
};

} // namespace bv
