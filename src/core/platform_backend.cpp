#include "core/platform_backend.h"

namespace baniphelper::core {

bool PlatformBackend::isComplete() const {
  return privilege != nullptr && singleInstance != nullptr && filterEngine != nullptr &&
         killer != nullptr && paths != nullptr && connMonitor != nullptr &&
         trafficStats != nullptr && capabilities != nullptr && !name.isEmpty();
}

}  // namespace baniphelper::core
