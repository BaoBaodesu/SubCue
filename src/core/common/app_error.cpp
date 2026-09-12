#include "common/app_error.h"

#include <utility>

namespace subcue {

AppError::AppError(ErrorDomain domain, int code, QString userMessage, QString technicalDetails)
    : domain_(domain),
      code_(code),
      userMessage_(std::move(userMessage)),
      technicalDetails_(std::move(technicalDetails))
{
}

} // namespace subcue
