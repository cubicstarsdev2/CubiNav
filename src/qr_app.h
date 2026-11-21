#pragma once
// QR code shown on the setup screen: opens the app's download page.
// Generated with segno: version 2, error correction M, 25 x 25 modules.
// PLACEHOLDER URL - put your real store / landing page here and regenerate. A longer URL needs a bigger QR version: change QR_APP_N and the sprite size in ui.cpp.
#include <stdint.h>
#define QR_APP_URL  "https://cubinav.app/get"
#define QR_APP_N    25
// Row y, module x is dark when bit (24 - x) of QR_APP_ROWS[y] is set.
static const uint32_t QR_APP_ROWS[QR_APP_N] = {
  0x1FD687F,  // #######.#.##.#....#######
  0x1040141,  // #.....#.........#.#.....#
  0x175BF5D,  // #.###.#.##.######.#.###.#
  0x174EC5D,  // #.###.#..###.##...#.###.#
  0x174985D,  // #.###.#..#..##....#.###.#
  0x1053041,  // #.....#.#..##.....#.....#
  0x1FD557F,  // #######.#.#.#.#.#.#######
  0x0004300,  // ..........#....##........
  0x1466525,  // #.#...##..##..#.#..#..#.#
  0x12B5AEB,  // #..#.#.##.#.##.#.###.#.##
  0x137534D,  // #..##.###.#.#..##.#..##.#
  0x1813788,  // ##......#..##.####...#...
  0x1A46261,  // ##.#..#...##...#..##....#
  0x08B8363,  // .#...#.###.....##.##...##
  0x1EFA2CD,  // ####.#####.#...#.##..##.#
  0x0518478,  // ..#.#...##....#...####...
  0x1B7F3F2,  // ##.##.#######..######..#.
  0x0011111,  // ........#...#...#...#...#
  0x1FDD951,  // #######.###.##..#.#.#...#
  0x1043713,  // #.....#....##.###...#..##
  0x17413F1,  // #.###.#.....#..######...#
  0x1744596,  // #.###.#...#...#.##..#.##.
  0x17538BB,  // #.###.#.#..###...#.###.##
  0x104C170,  // #.....#..##.....#.###....
  0x1FD7D49,  // #######.#.#####.#.#..#..#
};