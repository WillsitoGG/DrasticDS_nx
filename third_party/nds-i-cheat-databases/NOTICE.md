# DraStic-compatible merged Nintendo DS cheat database

`usrcheat.dat` keeps all 3,204 records from the CMP NDS Cheat Database 220713
shipped in the known-working DrasticDS_nx 1.0.6 release. It appends only game
IDs absent from that base, using records from DeadSkullzJr's August 12, 2025
NDS(i) Cheat Database. Existing CMP records are never replaced or rewritten.

- Compatible base size: 13,739,796 bytes
- Compatible base SHA-256: `a69264226312f584caf43343d27f0d38213d4fb01758ca01880f509c98fbeb29`
- Update author and maintainer: DeadSkullzJr
- Update source: https://gbatemp.net/threads/deadskullzjrs-nds-i-cheat-databases.488711/
- Update release date: 2025-08-12
- Update source size: 55,503,268 bytes
- Update source SHA-256: `9de3e6be8e54d4a46dc39881d1e1da7d42b2ce661afcb43d9fac27e1be48ec57`
- Added records: 1,137 (1,131 unique game IDs)
- Excluded records: 17 DSiWare records with `K`-prefix game codes
- Output records: 4,341
- Output size: 46,647,560 bytes
- Output SHA-256: `a8f96ed21f8921a5580d8f9d2850ffcb978af3a555a4e24565f6ebe9f46db4d3`
- DeadSkullzJr update license: GNU Affero General Public License v3.0 (`COPYING`)

The merged output preserves the complete 1.0.6 header and every base record
byte-for-byte, then copies whole missing update records without interpreting or
rewriting their folders, notes, or Action Replay codes. DSi-enhanced retail
cartridges are retained because DraStic can run their Nintendo DS-compatible
mode; DSiWare is omitted. The input and output hashes above document the exact
merge provenance.
