/* =========================================================================================

   This is an auto-generated file: Any edits you make may be overwritten!

*/

#pragma once

namespace BinaryData
{
    extern const char*   rewindbtn_svg;
    const int            rewindbtn_svgSize = 446;

    extern const char*   playbtn_svg;
    const int            playbtn_svgSize = 376;

    extern const char*   pausebtn_svg;
    const int            pausebtn_svgSize = 468;

    extern const char*   stopbtn_svg;
    const int            stopbtn_svgSize = 396;

    extern const char*   fastforwardbtn_svg;
    const int            fastforwardbtn_svgSize = 450;

    extern const char*   loop_svg;
    const int            loop_svgSize = 678;

    extern const char*   folder2open_svg;
    const int            folder2open_svgSize = 619;

    extern const char*   volumeoff_svg;
    const int            volumeoff_svgSize = 357;

    extern const char*   volumeup_svg;
    const int            volumeup_svgSize = 750;

    extern const char*   volumemute_svg;
    const int            volumemute_svgSize = 551;

    extern const char*   _1circle_svg;
    const int            _1circle_svgSize = 279;

    extern const char*   _2circle_svg;
    const int            _2circle_svgSize = 477;

    extern const char*   _4circle_svg;
    const int            _4circle_svgSize = 421;

    extern const char*   _8circle_svg;
    const int            _8circle_svgSize = 717;

    extern const char*   PlayBold_ttf;
    const int            PlayBold_ttfSize = 194848;

    // Number of elements in the namedResourceList and originalFileNames arrays.
    const int namedResourceListSize = 15;

    // Points to the start of a list of resource names.
    extern const char* namedResourceList[];

    // Points to the start of a list of resource filenames.
    extern const char* originalFilenames[];

    // If you provide the name of one of the binary resource variables above, this function will
    // return the corresponding data and its size (or a null pointer if the name isn't found).
    const char* getNamedResource (const char* resourceNameUTF8, int& dataSizeInBytes);

    // If you provide the name of one of the binary resource variables above, this function will
    // return the corresponding original, non-mangled filename (or a null pointer if the name isn't found).
    const char* getNamedResourceOriginalFilename (const char* resourceNameUTF8);
}
