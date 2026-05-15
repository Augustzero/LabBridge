#pragma once

#include "labbridge/agent/parser.h"

namespace labbridge::agent {

class CsvObservationParser final : public IParser {
public:
    ParseResult parse(const RawFileContext& context) override;
};

}  // namespace labbridge::agent

