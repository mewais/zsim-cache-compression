# Cache Compression zsim
This is a variation of the original [zsim simulator](https://github.com/s5z/zsim) that supports compressed caches. I built this in collaboration with [Amin Ghasemazar](https://github.com/Amin-Azar) at UBC, under the supervision of Prof. [Mieszko Lis](http://mieszko.ece.ubc.ca/). This work was part of my [MSc thesis](https://open.library.ubc.ca/cIRcle/collections/ubctheses/24/items/1.0368685) and an updated version of this simulator has been used in two accepted papers that are yet to appear.

## Caches
This simulator includes different types of compressed caches:
- [Doppelganger Cache](https://ieeexplore.ieee.org/document/7856587)
- [Base Delta Immediate Cache](https://ieeexplore.ieee.org/document/7842950)
- [Deduplication Cache](https://dl.acm.org/doi/10.1145/2597652.2597655), and an ideal version of deduplication cache that knows exactly what to deduplicate without the use of hashing.
- A combined base delta immediate + deduplication cache as art of our work, and an ideal version similar to the one with deduplication cache.
- An approximate variant of all of those (Doppelganger is approximate by default)
