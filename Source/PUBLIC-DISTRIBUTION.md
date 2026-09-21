# VO-VE 0.2.19 Distribution

Application source commit: cdbf09665d6cd67edd0b4b269fbb8fde4a497f62.
Build label: CWS-DND-20260922.

The uploaded packages are Windows Setup and Linux DEB, with SHA256SUMS.txt.
For the Source/ migration, the public snapshot belongs in Source/ in the release tag's
automatic ZIP and TAR.GZ archives.
Until those archives pass content verification and full Windows/Linux builds without WORK,
the release also retains the custom VO-VE-0.2.19-sources.zip and local source delivery.
Unchanged dependency supplements are reused from a pinned public
release; they are not duplicate uploads or entries in this release's SHA256SUMS.txt:

- [VO-VE-0.2.13-dependency-sources.zip](https://github.com/dercar2/VO-VE/releases/download/v0.2.16/VO-VE-0.2.13-dependency-sources.zip)
  SHA-256: 15bb49efffa446a98c413b7554b50e445ba505a0472acf112e6ff96e62ca2a46
- [VO-VE-0.2.13-notices.zip](https://github.com/dercar2/VO-VE/releases/download/v0.2.16/VO-VE-0.2.13-notices.zip)
  SHA-256: da6972dcbd55266931e72fc4992fc395e4f531e033d9e3a1bf4254303db9d946

The supplements retain their original baseline version and provenance. Dependency versions,
integration patches and build inputs are unchanged; their baseline references do not identify
the new application source. Use the sources for this release tag, not the supplement baseline.

Windows now also ships Qt 6.10.3 tls/qschannelbackend.dll, SHA-256 21F87B16F178FF625309D7CE6AE263B7DF320FBFC210F50D60667C8A4C9301AE.
It uses Windows Schannel and the system certificate store; OpenSSL DLLs are not added.
The exact plugin source is in qtbase-everywhere-src-6.10.3/src/plugins/tls/schannel within
the full Qt source archive already supplied. Qt's original notices cover this component.
Older runtime indexes observe the baseline DLL set; this paragraph records the added plugin.

VO-VE-authored code is GPL-3.0-or-later. MuPDF is AGPL-3.0-or-later, not commercially licensed.
Third-party components retain their original licenses. The companion notices and exact dependency
sources are available through the pinned links above without requiring an account. Keep applicable notices
and corresponding-source delivery when redistributing. This is not a legal certification.

Start with BUILDING.md. This lean source snapshot excludes developer tests, benchmarks,
experimental interfaces, reports and tool caches, not production build definitions or licenses.
Compiler/toolchain and artwork authoring inputs are provisioned separately as described there.
Automatic tag archives also contain the public README/site outside Source/.
Their compressed-byte hashes are verification evidence, not permanent uploaded-asset checksums.
