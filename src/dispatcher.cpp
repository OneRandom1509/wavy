/************************************************
 * Wavy Project - High-Fidelity Audio Streaming
 * ---------------------------------------------
 * 
 * Copyright (c) 2025 Oinkognito
 * All rights reserved.
 * 
 * This source code is part of the Wavy project, an advanced
 * local networking solution for high-quality audio streaming.
 * 
 * License:
 * This software is licensed under the BSD-3-Clause License.
 * You may use, modify, and distribute this software under
 * the conditions stated in the LICENSE file provided in the
 * project root.
 * 
 * Warranty Disclaimer:
 * This software is provided "AS IS," without any warranties
 * or guarantees, either expressed or implied, including but
 * not limited to fitness for a particular purpose.
 * 
 * Contributions:
 * Contributions to this project are welcome. By submitting 
 * code, you agree to license your contributions under the 
 * same BSD-3-Clause terms.
 * 
 * See LICENSE file for full details.
 ************************************************/

#include <libwavy/dispatch/entry.hpp>

auto main(int argc, char* argv[]) -> int
{
  logger::init_logging();

  if (argc < 5)
  {
    LOG_ERROR << "Usage: " << argv[0] << " <server> <port> <directory> <master_playlist>";
    return 1;
  }

  std::string server          = argv[1];
  std::string port            = argv[2];
  std::string dir             = argv[3];
  std::string master_playlist = argv[4];

  try
  {
    libwavy::dispatch::Dispatcher dispatcher(server, port, dir, master_playlist);
    if (!dispatcher.process_and_upload())
    {
      LOG_ERROR << DISPATCH_LOG << "Upload process failed.";
      return 1;
    }

    LOG_INFO << DISPATCH_LOG << "Upload successful.";
    return 0;
  }
  catch (const std::exception& e)
  {
    LOG_ERROR << "[Main] Error: " << e.what();
    return 1;
  }
}
