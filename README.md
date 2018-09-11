About
-----

I'm trying to implement the H264 decoder part of the RK3288 VPU driver,
using **Ezequiel Garcia** JPEG encoder as a basis.

I'm testing this code against my [RockMyy](../RockMyy) branch
(4.19-rc3), which contains the various kernel-side V4L2 patches
required to compile and use this driver.

Note that on [RockMyy](../RockMyy), I put videobuf2-dma-contig as a
module, which needs to be loaded before loading this driver.
