module.exports = {
  "branches": "main",
  "tagFormat": "v${version}",
  "plugins": [
    [
      "@semantic-release/commit-analyzer", {
        "preset": "conventionalcommits",
        "releaseRules": [
          {"breaking": true, "release": "minor"},
          {"type": "feat", "release": "minor"},
          {"type": "fix", "release": "patch"},
          {"type": "perf", "release": "patch"},
          {"type": "refactor", "release": "patch"},
          {"type": "docs", "release": "patch"},
          {"type": "build", "release": "patch"}
        ]
      }
    ],
    [
      "@semantic-release/release-notes-generator", {
        "preset": "conventionalcommits",
        "presetConfig": {
          "types": [
            {"type": "feat", "section": "Features"},
            {"type": "fix", "section": "Bug Fixes"},
            {"type": "refactor", "section": "Refactoring"},
            {"type": "perf", "section": "Performance improvements"}
          ]
        }
      }
    ],
    "@semantic-release/changelog",
    [
      "@semantic-release/exec", {
        // The component version lives in project(... VERSION x.y.z) and is published as
        // the HWLIB_VERSION target property, which dependants check. A tag whose sources
        // still carry the previous number would make that check lie.
        "prepareCmd": "sed -i 's/^  VERSION .*/  VERSION ${nextRelease.version}/' CMakeLists.txt && grep -q '^  VERSION ${nextRelease.version}$' CMakeLists.txt"
      }
    ],
    [
      "@semantic-release/git", {
        "assets": ["CHANGELOG.md", "CMakeLists.txt"],
        "message": "chore(release): ${lastRelease.version} -> ${nextRelease.version} [skip ci]"
      }
    ]
  ]
};
