## [1.2.0](https://github.com/aloknigam247/dot/compare/android-v1.1.1...android-v1.2.0) (2026-11-05)

### Added

* **ui:** resolve semantic desktop colors ([#235](https://github.com/aloknigam247/dot/issues/235)) ([3f22589](https://github.com/aloknigam247/dot/commit/3f22589142f95cfa0720e80d815b7eebf8419e2b)), closes [#102](https://github.com/aloknigam247/dot/issues/102)

### Fixed

* **ui:** show Neovim launch errors ([#238](https://github.com/aloknigam247/dot/issues/238)) ([da29d25](https://github.com/aloknigam247/dot/commit/da29d25cc39e1251614cc3bab8c3ee80845b8851)), closes [#95](https://github.com/aloknigam247/dot/issues/95)

## [1.1.1](https://github.com/aloknigam247/dot/compare/android-v1.1.0...android-v1.1.1) (2026-11-01)

### Fixed

* **ui:** disable cursor movement animation ([696bd5c](https://github.com/aloknigam247/dot/commit/696bd5c65a711a374db15b17dd06e15be3d8b3f1)), closes [#124](https://github.com/aloknigam247/dot/issues/124)
* **ui:** honor cursor geometry and live mode updates ([87a02ef](https://github.com/aloknigam247/dot/commit/87a02ef1f8b05926f56ec4625eb2ae8823317440))

## [1.1.0](https://github.com/aloknigam247/dot/compare/android-v1.0.0...android-v1.1.0) (2026-10-31)

### Added

* **ui:** add native Neovim image support ([a8acd06](https://github.com/aloknigam247/dot/commit/a8acd063e7e5fdda873e441e7162e1ac3901f859)), closes [#2](https://github.com/aloknigam247/dot/issues/2)

## 1.0.0 (2026-10-26)

### Added

* add dockable chat panel with echo-stub responder ([#45](https://github.com/aloknigam247/dot/issues/45)) ([c1996be](https://github.com/aloknigam247/dot/commit/c1996be80d9e50cdea4281850ec020cfe64ca143)), closes [#28](https://github.com/aloknigam247/dot/issues/28)
* Android companion app skeleton mirroring qvim chat over WebSocket ([#56](https://github.com/aloknigam247/dot/issues/56)) ([ec40e8c](https://github.com/aloknigam247/dot/commit/ec40e8c181b58482fc24491c2762af5c573aa8ff)), closes [#49](https://github.com/aloknigam247/dot/issues/49) [#48](https://github.com/aloknigam247/dot/issues/48) [#29](https://github.com/aloknigam247/dot/issues/29)
* initial qvim — Neovim GUI client in C++23 / Qt 6.10 / QML ([aca52dd](https://github.com/aloknigam247/dot/commit/aca52dd573ece5887d3ea53ea6e385f91eaf9bed))
* mDNS LAN discovery for the session mirror ([#62](https://github.com/aloknigam247/dot/issues/62)) ([1385eed](https://github.com/aloknigam247/dot/commit/1385eed3ceb43ab7f4cde1201e3e9f2f1d01b86b)), closes [#50](https://github.com/aloknigam247/dot/issues/50) [#29](https://github.com/aloknigam247/dot/issues/29)
* **release:** version monorepo components independently ([c9b8e62](https://github.com/aloknigam247/dot/commit/c9b8e62152810da437664b805b88c11500946010))
* replace android chat backend with ahp client ([#93](https://github.com/aloknigam247/dot/issues/93)) ([731629a](https://github.com/aloknigam247/dot/commit/731629ab31da8d202b8827a0f14f1a29618f97b6))

### Changed

* **launch:** exclude whole build tree from Defender, not just deploy dir ([#46](https://github.com/aloknigam247/dot/issues/46)) ([08b47ea](https://github.com/aloknigam247/dot/commit/08b47ea152dabaf2c8027e4a612a7a1e01b3d0c7))
* **launch:** trim Qt deploy and add Defender-exclusion helper for cold start ([#39](https://github.com/aloknigam247/dot/issues/39)) ([f7e87a4](https://github.com/aloknigam247/dot/commit/f7e87a4b2d7fa983ef4fb07134a58de23b47b6de)), closes [#18](https://github.com/aloknigam247/dot/issues/18)
