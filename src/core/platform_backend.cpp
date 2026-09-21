#include "core/platform_backend.h"

namespace baniphelper::core {

bool PlatformBackend::isComplete() const {
  return privilege != nullptr && singleInstance != nullptr && capabilities != nullptr &&
         !name.isEmpty();
}

}  // namespace baniphelper::core
