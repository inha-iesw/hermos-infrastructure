// Copyright 2026
// SPDX-License-Identifier: Apache-2.0

#include "hermos/internal/type_registry_writer.hpp"

#include <unistd.h>

#include <iostream>

int main()
{
  auto & writer = hermos::internal::TypeRegistryWriter::instance();
  writer.register_type("/registry_contract", "std_msgs/msg/String", "pub", "/writer");
  writer.register_type("/registry_contract", "std_msgs/msg/String", "sub", "/reader");
  // Keep the real PID/start identity alive until the reader finishes its assertions.
  std::cout << getpid() << std::endl;
  return std::cin.get() == 'q' ? 0 : 1;
}
