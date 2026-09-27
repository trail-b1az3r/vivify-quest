# AudioLink.bundle

AudioLink's analysis shader, material and CustomRenderTexture for the Quest,
linked into libVivify.so by `src/VivifyAudioLinkBundle.cpp` and driven by
`src/VivifyAudioLink.cpp`.

Built by `tools/audiolink/build_bundle.py` from `AudioLink/Assets/Bundle2021`
of [Aeroluna/BSAudioLink](https://github.com/Aeroluna/BSAudioLink) (commit
0b127ea, AudioLink 3.1.2): the shader's DirectX programs translated to GLES by
this mod's converter, and the texture set to update in realtime.

Licences: BSAudioLink is MIT (c) 2022 Aeroluna; AudioLink
([llealloo/vrc-udon-audio-link](https://github.com/llealloo/vrc-udon-audio-link))
is MIT (c) llealloo and contributors.
