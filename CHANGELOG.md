Changelog
1.2.0
Added
Standard and Extended detail levels for photos/RAW files, video/audio files, and other file types.
Photo/RAW metadata including camera, lens, ISO, aperture, shutter speed, and focal length when exposed by Windows.
Video frame rate and audio sample rate/channel information when exposed by Windows.
Font family and font-size controls.
Configurable left-edge padding and spacing between sections.
Four directional gradient panel fills for Flat panes and Soft cards.
Improved
Single-file details now use compact middle-dot-separated formatting.
RAW metadata handling was validated with NEF, DNG, CR2, and CR3 sample files.
Metadata caching now also tracks file size so replaced or changed files are less likely to show stale details.
Drive free-space caching avoids unnecessary refreshes when navigating between folders on the same drive.
Worker refresh logic skips fresh unrelated Explorer windows after selection events.
Video and audio property queries are separated so unsupported properties are not queried unnecessarily.
Fixed
Corrected low-sample-rate audio formatting.
Improved metadata failure handling so unavailable optional properties do not cause needless retries.
Improved selection/cache refresh behavior for single-item selections and folder changes.