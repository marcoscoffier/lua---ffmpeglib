
package = "ffmpeglib"
version = "1.0-1"

source = {
   url = "ffmpeglib-1.0-1.tgz"
}

description = {
   summary = "Provides a Video class, interfacing ffmpeg",
   detailed = [[
         Decodes video frames via ffmpeg, and uses the
         torch.Tensor class (from Torch7) to store them.
         Also uses the qt package to display the videos.
   ]],
   homepage = "",
   license = "MIT/X11" -- or whatever you like
}

dependencies = {
   "lua >= 5.1",
   "torch"
}

build = {
   type = "cmake", 
   variables = {
      CMAKE_INSTALL_PREFIX = "$(PREFIX)"
   }
}
