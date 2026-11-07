# cascade for vcpkg. The release workflow puts this port, with the SHA512 of the release's sources
# filled in, in each release (cascade-<version>-vcpkg-port.zip): an overlay port, or what to send
# to microsoft/vcpkg. CI builds it from the checkout (packaging/vcpkg/testPort.sh).
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO eugeniobenfica/cascade
    REF "v${VERSION}"
    SHA512 0
    HEAD_REF main
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        window          CASCADE_WINDOW
        vulkan          CASCADE_VULKAN
        opengl          CASCADE_OPENGL
        accessibility   CASCADE_ACCESSIBILITY
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DCASCADE_BUILD_EXAMPLES=OFF
        -DCASCADE_BUILD_TOOLS=OFF
        -DCASCADE_BUILD_TESTS=OFF
        -DCASCADE_INSTALL=ON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/cascade)
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/doc")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
