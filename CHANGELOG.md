Changelog

1.2.0


Added:

-Standard and Extended detail levels for photos/RAW files, video/audio files, and other file types.

-Photo/RAW metadata including camera, lens, ISO, aperture, shutter speed, and focal length when available through Windows.

-Video frame rate and audio sample rate/channel information when available through Windows.

-Font family and font-size controls.

-Configurable left padding and spacing between sections.

-Four directional gradient panel fills for Flat panes and Soft cards.


Improved:

-Single-file details now use compact middle-dot-separated formatting.

-RAW metadata handling was validated with NEF, DNG, CR2, and CR3 sample files.

-Metadata caching now also tracks file size so replaced or changed files are less likely to show stale details.

-Drive free-space caching avoids unnecessary refreshes when navigating between folders on the same drive.

-Worker refresh logic skips fresh unrelated Explorer windows after selection events.

-Video and audio property queries are separated so unsupported properties are not queried unnecessarily.

-Reduced unnecessary filesystem access when resolving Windows file type descriptions.

-Improved responsiveness during metadata shutdown and cancellation.

-Improved font refresh handling when settings are changed.


Fixed:

-Corrected low-sample-rate audio formatting.

-Improved metadata failure handling so unavailable optional properties do not cause needless retries.

-Improved selection/cache refresh behavior for single-item selections and folder changes.

-Fixed custom left-padding and section-spacing dropdown values not always applying correctly.

-Fixed a settings-refresh edge case that could trigger redundant selection refreshes.

- Fixed single-file metadata remaining visible after deselecting a file.

- Fixed leftover file-detail content when switching to a file with shorter metadata.



1.1.0

Added:

-Added an option to hide File Explorer's native view buttons at the bottom-right of the info bar.

1.0.0

-Initial release.