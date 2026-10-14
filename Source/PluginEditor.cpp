#include "PluginEditor.h"
#include "BinaryData.h"

namespace
{
    const juce::Colour background { 0xff0d0d0d };
    const juce::Colour panel { 0xff171717 };
    const juce::Colour border { 0xff303030 };
    const juce::Colour textPrimary { 0xfff0f0f0 };
    const juce::Colour textSecondary { 0xff939393 };
    const juce::Colour accent { 0xffd9d9d9 };
    const juce::Colour errorColour { 0xffececec };
    constexpr int editorWidth = 600;
    constexpr int editorHeight = 360;
    constexpr int horizontalMargin = 12;
    constexpr int headingHeight = 28;
    constexpr int svgCanvasSize = 16;

    juce::Font interfaceFont (const juce::Typeface::Ptr& typeface, float height)
    {
        return juce::Font (typeface).withHeight (height);
    }

    std::unique_ptr<juce::Drawable> svgIcon (const char* data, int size)
    {
        auto xml = juce::XmlDocument::parse (juce::String::fromUTF8 (data, size).replace ("currentColor", "#000000"));
        if (xml != nullptr)
        {
            // SVGs without explicit dimensions otherwise inherit JUCE's default viewport.
            xml->setAttribute ("width", svgCanvasSize);
            xml->setAttribute ("height", svgCanvasSize);
        }
        auto drawable = xml != nullptr ? juce::Drawable::createFromSVG (*xml) : nullptr;
        jassert (drawable != nullptr);
        return drawable;
    }

    void drawSvgIcon (juce::Graphics& g, const juce::Drawable& icon, juce::Rectangle<float> bounds,
                      float opacity = 1.0f)
    {
        // Preserve the shared SVG canvas instead of fitting each icon's visible paths.
        const juce::Rectangle<float> viewBox { 0.0f, 0.0f, static_cast<float> (svgCanvasSize),
                                                           static_cast<float> (svgCanvasSize) };
        icon.draw (g, opacity, juce::RectanglePlacement (juce::RectanglePlacement::centred)
                                  .getTransformToFit (viewBox, bounds));
    }

    juce::String formatTime (double seconds)
    {
        const auto milliseconds = static_cast<juce::int64> (std::llround (juce::jmax (0.0, seconds) * 1000.0));
        const auto hours = milliseconds / 3600000;
        const auto minutes = (milliseconds / 60000) % 60;
        const auto wholeSeconds = (milliseconds / 1000) % 60;
        const auto fraction = milliseconds % 1000;
        const auto twoDigits = [] (juce::int64 value) { return juce::String (value).paddedLeft ('0', 2); };
        const auto time = twoDigits (minutes) + ":" + twoDigits (wholeSeconds)
                        + "." + juce::String (fraction).paddedLeft ('0', 3);
        return hours > 0 ? juce::String (hours) + ":" + time : time;
    }

    void drawPanel (juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        g.setColour (panel);
        g.fillRoundedRectangle (bounds, 10.0f);
        g.setColour (border);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 10.0f, 1.0f);
    }
}

class WaveformPlayerAudioProcessorEditor::PlayerLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    explicit PlayerLookAndFeel (juce::Typeface::Ptr typeface) : interfaceTypeface (std::move (typeface))
    {
        setColour (juce::TooltipWindow::backgroundColourId, panel);
        setColour (juce::TooltipWindow::textColourId, textPrimary);
        setColour (juce::TooltipWindow::outlineColourId, border);
        setColour (juce::Slider::backgroundColourId, border);
        setColour (juce::Slider::trackColourId, accent);
        setColour (juce::Slider::thumbColourId, textPrimary);
        loadIcon (0, "rewind", BinaryData::rewindbtn_svg, BinaryData::rewindbtn_svgSize);
        loadIcon (1, "play", BinaryData::playbtn_svg, BinaryData::playbtn_svgSize);
        loadIcon (2, "pause", BinaryData::pausebtn_svg, BinaryData::pausebtn_svgSize);
        loadIcon (3, "stop", BinaryData::stopbtn_svg, BinaryData::stopbtn_svgSize);
        loadIcon (4, "forward", BinaryData::fastforwardbtn_svg, BinaryData::fastforwardbtn_svgSize);
        loadIcon (5, "loop", BinaryData::loop_svg, BinaryData::loop_svgSize);
        loadIcon (6, "open", BinaryData::folder2_svg, BinaryData::folder2_svgSize);
        loadIcon (7, "speed1", BinaryData::_1circle_svg, BinaryData::_1circle_svgSize);
        loadIcon (8, "speed2", BinaryData::_2circle_svg, BinaryData::_2circle_svgSize);
        loadIcon (9, "speed4", BinaryData::_4circle_svg, BinaryData::_4circle_svgSize);
        loadIcon (10, "speed8", BinaryData::_8circle_svg, BinaryData::_8circle_svgSize);
        loadIcon (11, "volume-up", BinaryData::volumeup_svg, BinaryData::volumeup_svgSize);
        loadIcon (12, "volume-mute", BinaryData::volumemute_svg, BinaryData::volumemute_svgSize);
        loadIcon (13, "volume-off", BinaryData::volumeoff_svg, BinaryData::volumeoff_svgSize);
        loadIcon (14, "folder-open", BinaryData::folder2open_svg, BinaryData::folder2open_svgSize);
    }

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override { return interfaceTypeface; }

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override {}

    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool down) override
    {
        const auto isMuteButton = button.getComponentID() == "mute";
        const auto highlightedOrActive = highlighted || button.hasKeyboardFocus (true)
                                      || (button.getToggleState() && ! isMuteButton);
        const auto opacity = button.isEnabled() ? highlightedOrActive ? 1.0f : 0.85f : 0.25f;
        const auto isFolderButton = button.getComponentID() == "open";
        const auto iconId = isMuteButton ? juce::String (button.getToggleState() ? "volume-mute" : "volume-up")
                         : isFolderButton ? juce::String (button.getToggleState() ? "folder-open" : "open")
                                         : button.getComponentID();
        for (const auto& icon : icons)
        {
            if (icon.id == iconId && icon.normal != nullptr)
            {
                auto* drawable = highlightedOrActive ? icon.active.get() : icon.normal.get();
                const auto centreX = static_cast<float> (button.getWidth()) * 0.5f;
                const auto centreY = static_cast<float> (button.getHeight()) * 0.5f + (down ? 1.0f : 0.0f);
                const auto size = static_cast<float> (svgCanvasSize);
                drawSvgIcon (g, *drawable, { centreX - size * 0.5f, centreY - size * 0.5f, size, size }, opacity);
                if (button.getComponentID() == "loop" && button.getToggleState())
                {
                    g.setColour (textPrimary.withAlpha (opacity));
                    g.fillEllipse (centreX - 1.5f, static_cast<float> (button.getHeight()) - 4.0f, 3.0f, 3.0f);
                }
                return;
            }
        }
        g.setColour (textPrimary.withMultipliedAlpha (opacity));
        g.setFont (interfaceFont (interfaceTypeface, 12.0f));
        g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (8, 0),
                          juce::Justification::centred, 1);
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPosition,
                                           juce::Rectangle<int> parentArea) override
    {
        const auto layout = tooltipLayout (text);
        const auto width = juce::roundToInt (std::ceil (layout.getWidth())) + 20;
        const auto height = juce::roundToInt (std::ceil (layout.getHeight())) + 14;
        return juce::Rectangle<int> (screenPosition.x > parentArea.getCentreX() ? screenPosition.x - width - 12
                                                                             : screenPosition.x + 24,
                                      screenPosition.y > parentArea.getCentreY() ? screenPosition.y - height - 6
                                                                              : screenPosition.y + 6,
                                      width, height).constrainedWithin (parentArea);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        g.fillAll (panel);
        g.setColour (border);
        g.drawRect (0, 0, width, height);
        tooltipLayout (text).draw (g, juce::Rectangle<float> (static_cast<float> (width),
                                                             static_cast<float> (height)).reduced (10.0f, 7.0f));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosition, float, float,
                           const juce::Slider::SliderStyle, juce::Slider& slider) override
    {
        const auto centreY = static_cast<float> (y) + static_cast<float> (height) * 0.5f;
        const auto opacity = slider.isEnabled() ? 1.0f : 0.35f;
        g.setColour (border);
        g.fillRoundedRectangle (static_cast<float> (x), centreY - 2.0f, static_cast<float> (width), 4.0f, 2.0f);
        g.setColour (accent.withAlpha (opacity));
        g.fillRoundedRectangle (static_cast<float> (x), centreY - 2.0f,
                                juce::jmax (0.0f, sliderPosition - static_cast<float> (x)), 4.0f, 2.0f);
        g.setColour (textPrimary.withAlpha (opacity));
        g.fillEllipse (sliderPosition - 6.0f, centreY - 6.0f, 12.0f, 12.0f);
        g.setColour (background);
        g.fillEllipse (sliderPosition - 2.0f, centreY - 2.0f, 4.0f, 4.0f);
    }

private:
    struct Icon
    {
        juce::String id;
        std::unique_ptr<juce::Drawable> normal, active;
    };

    void loadIcon (size_t index, const juce::String& id, const char* data, int size)
    {
        auto drawable = svgIcon (data, size);
        if (drawable == nullptr)
            return;

        auto& icon = icons[index];
        icon.id = id;
        icon.normal = drawable->createCopy();
        icon.active = drawable->createCopy();
        icon.normal->replaceColour (juce::Colours::black, id == "loop" ? textSecondary : textPrimary);
        icon.active->replaceColour (juce::Colours::black, textPrimary);
    }

    juce::TextLayout tooltipLayout (const juce::String& text) const
    {
        juce::AttributedString attributed;
        attributed.append (text, interfaceFont (interfaceTypeface, 13.0f), textPrimary);
        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths (attributed, 440.0f);
        return layout;
    }

    juce::Typeface::Ptr interfaceTypeface;
    std::array<Icon, 15> icons;
};

class WaveformPlayerAudioProcessorEditor::WaveformDisplay final : public juce::Component,
                                                                 public juce::SettableTooltipClient,
                                                                 private juce::ChangeListener
{
public:
    explicit WaveformDisplay (juce::Typeface::Ptr typeface)
        : interfaceTypeface (std::move (typeface)), thumbnail (512, formatManager, thumbnailCache)
    {
        formatManager.registerBasicFormats();
        thumbnail.addChangeListener (this);
        setTooltip ("Left-click or drag to seek. Right-click to set A/B; Shift-right-click to clear.");
        setTitle ("Audio waveform");
        setDescription ("Click or drag to change the playback position");
        fileNameLabel.setFont (interfaceFont (interfaceTypeface, 14.0f));
        fileNameLabel.setColour (juce::Label::textColourId, textPrimary);
        fileStatusLabel.setFont (interfaceFont (interfaceTypeface, 11.5f));
        fileStatusLabel.setColour (juce::Label::textColourId, textSecondary);
        for (auto* label : { &fileNameLabel, &fileStatusLabel })
        {
            label->setBorderSize (juce::BorderSize<int> (0));
            addAndMakeVisible (*label);
        }
    }

    ~WaveformDisplay() override
    {
        thumbnail.removeChangeListener (this);
        thumbnail.clear();
    }

    void update (const WaveformPlayerAudioProcessor::PlaybackSnapshot& snapshot)
    {
        if (snapshot.loaded && (snapshot.file != displayedFile || snapshot.fileRevision != displayedRevision))
        {
            dragging = false;
            displayedFile = snapshot.file;
            displayedRevision = snapshot.fileRevision;
            thumbnail.clear();
            thumbnailCache.clear();
            // AudioThumbnail performs decoding and file reads on its background thread.
            thumbnailAvailable = thumbnail.setSource (new juce::FileInputSource (displayedFile, true));
        }

        loaded = snapshot.loaded;
        loading = snapshot.loading;
        choosingFile = snapshot.choosingFile;
        if (! loaded || loading || choosingFile)
            dragging = false;
        duration = snapshot.durationSeconds;
        sampleRate = snapshot.fileSampleRate;
        const auto markersChanged = markerSamples != snapshot.markerSamples;
        markerSamples = snapshot.markerSamples;
        rangeStart = snapshot.rangeStartSeconds;
        rangeEnd = snapshot.rangeEndSeconds;
        playbackSpeed = snapshot.playbackSpeed;
        if (! dragging || markersChanged)
            position = snapshot.positionSeconds;
        else
            position = juce::jlimit (rangeStart, rangeEnd, position);
        fileNameLabel.setText (snapshot.loaded ? snapshot.fileName : "No audio file loaded", juce::dontSendNotification);
        const auto filePath = snapshot.loaded ? snapshot.file.getFullPathName() : juce::String();
        fileNameLabel.setTooltip (filePath);
        fileNameLabel.setDescription (filePath);
        const auto status = snapshot.error.isNotEmpty() ? snapshot.error
                          : snapshot.status.isNotEmpty() ? snapshot.status : "Choose a WAV, MP3 or FLAC file";
        fileStatusLabel.setText (status, juce::dontSendNotification);
        fileStatusLabel.setTooltip (status);
        fileStatusLabel.setDescription (status);
        fileStatusLabel.setColour (juce::Label::textColourId, snapshot.error.isNotEmpty() ? errorColour : textSecondary);
        setMouseCursor (loaded && ! loading && ! choosingFile ? juce::MouseCursor::LeftRightResizeCursor
                                           : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void resized() override
    {
        auto heading = getLocalBounds().reduced (16, 6).removeFromTop (22);
        heading.removeFromRight (timeWidth + 16);
        fileNameLabel.setBounds (heading);
        auto status = getLocalBounds().reduced (16, 0).withTrimmedTop (28).removeFromTop (18);
        status.removeFromRight (timeWidth + 16);
        fileStatusLabel.setBounds (status);
    }

    void paint (juce::Graphics& g) override
    {
        drawPanel (g, getLocalBounds().toFloat());
        auto heading = getLocalBounds().reduced (16, 6).removeFromTop (22);
        g.setFont (interfaceFont (interfaceTypeface, 15.0f));
        g.setColour (loaded ? textPrimary : textSecondary);
        g.drawText (formatTime (position) + " / " + formatTime (duration),
                    heading.removeFromRight (timeWidth), juce::Justification::centredRight);

        const auto channelCount = juce::jmax (1, thumbnail.getNumChannels());
        auto information = getLocalBounds().reduced (16, 0).withTrimmedTop (28).removeFromTop (18);
        g.setFont (interfaceFont (interfaceTypeface, 11.0f));
        g.setColour (textSecondary);
        g.drawText (loaded ? juce::String (sampleRate / 1000.0, 1) + juce::String::fromUTF8 (" kHz · ")
                            + juce::String::fromUTF8 (channelCount == 1 ? "Mono" : "Stereo · L Channel")
                            + juce::String::fromUTF8 (" · ") + juce::String (playbackSpeed)
                            + juce::String::fromUTF8 ("×")
                          : juce::String ("WAVEFORM"),
                    information.removeFromRight (timeWidth), juce::Justification::centredRight);

        const auto bounds = getPlotBounds();
        g.setColour (panel);
        g.fillRoundedRectangle (bounds, 5.0f);
        g.setColour (border.withAlpha (0.65f));
        for (int division = 1; division < 8; ++division)
        {
            const auto x = bounds.getX() + bounds.getWidth() * static_cast<float> (division) / 8.0f;
            g.drawVerticalLine (juce::roundToInt (x), bounds.getY(), bounds.getBottom());
        }

        g.drawHorizontalLine (juce::roundToInt (bounds.getCentreY()), bounds.getX(), bounds.getRight());

        if (loaded && duration > 0.0)
        {
            const auto x = secondsToX (position);
            if (markerSamples[1] >= 0)
            {
                g.setColour (accent.withAlpha (0.12f));
                g.fillRect (bounds.withLeft (secondsToX (rangeStart)).withRight (secondsToX (rangeEnd)));
            }
            else
            {
                g.setColour (accent.withAlpha (0.055f));
                g.fillRect (bounds.withRight (x));
            }

            if (thumbnail.getNumChannels() > 0)
            {
                g.setColour (accent.withAlpha (loading ? 0.35f : 0.85f));
                // Only channel 0 is visualised; stereo audio output is unchanged.
                thumbnail.drawChannel (g, bounds.toNearestInt().reduced (0, 5), 0.0, duration, 0, 0.9f);
            }
            else
            {
                g.setColour (textSecondary);
                g.setFont (interfaceFont (interfaceTypeface, 14.0f));
                g.drawFittedText (juce::String::fromUTF8 (thumbnailAvailable ? "Building waveform…" : "Waveform unavailable"),
                                  bounds.toNearestInt(), juce::Justification::centred, 1);
            }

            g.setColour (textSecondary.withAlpha (loading ? 0.35f : 0.95f));
            g.setFont (interfaceFont (interfaceTypeface, 11.0f));
            for (size_t index = 0; index < markerSamples.size(); ++index)
            {
                if (markerSamples[index] < 0 || sampleRate <= 0.0)
                    continue;
                const auto markerX = secondsToX (static_cast<double> (markerSamples[index]) / sampleRate);
                const auto labelX = juce::jlimit (bounds.getX(), bounds.getRight() - 12.0f, markerX + 4.0f);
                g.drawLine (markerX, bounds.getY(), markerX, bounds.getBottom(), 1.25f);
                g.drawText (index == 0 ? "A" : "B",
                            juce::Rectangle<float> { labelX, bounds.getBottom() - 17.0f, 12.0f, 15.0f },
                            juce::Justification::centredLeft);
            }

            g.setColour (textPrimary.withAlpha (loading ? 0.35f : 0.95f));
            g.drawLine (x, bounds.getY(), x, bounds.getBottom(), 1.5f);
            juce::Path handle;
            handle.startNewSubPath (x - 5.0f, bounds.getY());
            handle.lineTo (x + 5.0f, bounds.getY());
            handle.lineTo (x + 5.0f, bounds.getY() + 5.0f);
            handle.lineTo (x, bounds.getY() + 10.0f);
            handle.lineTo (x - 5.0f, bounds.getY() + 5.0f);
            handle.closeSubPath();
            g.fillPath (handle);
        }
        else
        {
            g.setColour (textSecondary);
            g.setFont (interfaceFont (interfaceTypeface, 14.0f));
            g.drawFittedText (juce::String::fromUTF8 (loading ? "Loading audio…"
                              : loaded ? thumbnailAvailable ? "Building waveform…" : "Waveform unavailable"
                                       : "Open an audio file to begin"),
                              bounds.toNearestInt(), juce::Justification::centred, 1);
        }

        auto footer = getLocalBounds().reduced (16, 0).removeFromBottom (25);
        g.setFont (interfaceFont (interfaceTypeface, 11.0f));
        g.setColour (textSecondary);
        g.drawText (juce::String::fromUTF8 (loaded ? "Right-click: A/B · Shift-right-click: clear" : "WAV · MP3 · FLAC"), footer,
                    juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (loaded && ! loading && ! choosingFile && duration > 0.0 && getPlotBounds().contains (event.position))
        {
            if (event.mods.isRightButtonDown())
            {
                dragging = false;
                if (event.mods.isShiftDown())
                {
                    if (onClearMarkers)
                        onClearMarkers();
                }
                else if (onAddMarker)
                {
                    onAddMarker (xToSeconds (event.position.x));
                }
                repaint();
                return;
            }
            if (event.mods.isLeftButtonDown())
            {
                dragging = true;
                seek (event.position.x);
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (dragging)
            seek (event.position.x);
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    std::function<void (double)> onSeek;
    std::function<void (double)> onAddMarker;
    std::function<void()> onClearMarkers;

private:
    juce::Rectangle<float> getPlotBounds() const
    {
        return getLocalBounds().reduced (16, 0).withTrimmedTop (52).withTrimmedBottom (25).toFloat();
    }

    float secondsToX (double seconds) const
    {
        const auto bounds = getPlotBounds();
        const auto fraction = duration > 0.0 ? juce::jlimit (0.0, 1.0, seconds / duration) : 0.0;
        return bounds.getX() + static_cast<float> (fraction) * bounds.getWidth();
    }

    double xToSeconds (float x) const
    {
        const auto bounds = getPlotBounds();
        return duration * juce::jlimit (0.0, 1.0, static_cast<double> ((x - bounds.getX()) / bounds.getWidth()));
    }

    void seek (float x)
    {
        position = juce::jlimit (rangeStart, rangeEnd, xToSeconds (x));
        if (onSeek)
            onSeek (position);
        repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    juce::Typeface::Ptr interfaceTypeface;
    juce::Label fileNameLabel, fileStatusLabel;
    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache { 4 };
    juce::AudioThumbnail thumbnail;
    juce::File displayedFile;
    uint64_t displayedRevision = 0;
    static constexpr int timeWidth = 260;
    bool loaded = false, loading = false, choosingFile = false, dragging = false, thumbnailAvailable = false;
    int playbackSpeed = 1;
    double position = 0.0, duration = 0.0, sampleRate = 0.0;
    double rangeStart = 0.0, rangeEnd = 0.0;
    std::array<juce::int64, 2> markerSamples {{ -1, -1 }};
};

WaveformPlayerAudioProcessorEditor::WaveformPlayerAudioProcessorEditor (WaveformPlayerAudioProcessor& processor)
    : AudioProcessorEditor (&processor), audioProcessor (processor),
      interfaceTypeface (juce::Typeface::createSystemTypefaceFor (BinaryData::PlayBold_ttf,
                                                                 BinaryData::PlayBold_ttfSize)),
      playerLookAndFeel (std::make_unique<PlayerLookAndFeel> (interfaceTypeface)),
      waveform (std::make_unique<WaveformDisplay> (interfaceTypeface))
{
    jassert (interfaceTypeface != nullptr);
    setLookAndFeel (playerLookAndFeel.get());
    tooltips.setLookAndFeel (playerLookAndFeel.get());
    setOpaque (true);
    setResizable (false, false);

    titleLabel.setText ("Waveform Player", juce::dontSendNotification);
    titleLabel.setFont (interfaceFont (interfaceTypeface, 25.0f));
    subtitleLabel.setFont (interfaceFont (interfaceTypeface, 12.0f));
    subtitleLabel.setText ("Standalone audio playback", juce::dontSendNotification);
    subtitleLabel.setJustificationType (juce::Justification::centredRight);
    volumeValueLabel.setFont (interfaceFont (interfaceTypeface, 14.0f));
    volumeValueLabel.setJustificationType (juce::Justification::centredRight);
    for (auto* label : { &titleLabel, &subtitleLabel, &volumeValueLabel })
    {
        label->setColour (juce::Label::textColourId,
                          label == &subtitleLabel ? textSecondary : textPrimary);
        label->setBorderSize (juce::BorderSize<int> (0));
        addAndMakeVisible (*label);
    }

    configureButton (openButton, "Open", "Load a WAV, MP3 or FLAC audio file");
    openButton.setComponentID ("open");
    configureButton (rewindButton, juce::String::fromUTF8 ("RW  −5 s"), "Seek backward by five seconds");
    rewindButton.setComponentID ("rewind");
    configureButton (playButton, "Play", "Play from the current position");
    playButton.setComponentID ("play");
    configureButton (pauseButton, "Pause", "Pause and keep the current position");
    pauseButton.setComponentID ("pause");
    configureButton (stopButton, "Stop", "Stop and return to the start");
    stopButton.setComponentID ("stop");
    configureButton (forwardButton, "FF  +5 s", "Seek forward by five seconds");
    forwardButton.setComponentID ("forward");
    configureButton (loopButton, "Loop Off", "Repeat the whole audio file");
    loopButton.setComponentID ("loop");
    configureButton (speedButton, "Speed 1x", "Click to cycle playback speed: 1x, 2x, 4x, 8x");
    speedButton.setComponentID ("speed1");
    configureButton (muteButton, {}, "Mute the output without changing the volume setting");
    muteButton.setComponentID ("mute");
    configureButton (minimumVolumeButton, {}, "Set the output volume to its minimum: -60 dB");
    minimumVolumeButton.setComponentID ("volume-off");
    minimumVolumeButton.setName ("Set volume to minimum");
    minimumVolumeButton.setTitle (minimumVolumeButton.getName());
    loopButton.setClickingTogglesState (true);
    muteButton.setClickingTogglesState (true);

    openButton.onClick = [this] { audioProcessor.chooseAudioFile(); updateFromProcessor(); };
    rewindButton.onClick = [this] { audioProcessor.skipSeconds (-5.0); updateFromProcessor(); };
    playButton.onClick = [this] { audioProcessor.play(); updateFromProcessor(); };
    pauseButton.onClick = [this] { audioProcessor.pause(); updateFromProcessor(); };
    stopButton.onClick = [this] { audioProcessor.stop(); updateFromProcessor(); };
    forwardButton.onClick = [this] { audioProcessor.skipSeconds (5.0); updateFromProcessor(); };
    loopButton.onClick = [this] { audioProcessor.setLooping (loopButton.getToggleState()); updateFromProcessor(); };
    speedButton.onClick = [this] { audioProcessor.cyclePlaybackSpeed(); updateFromProcessor(); };
    muteButton.onClick = [this] { audioProcessor.setMuted (muteButton.getToggleState()); updateFromProcessor(); };
    minimumVolumeButton.onClick = [this]
    {
        const auto snapshot = audioProcessor.getPlaybackSnapshot();
        if (! snapshot.loaded || snapshot.loading || snapshot.choosingFile || audioProcessor.isMuted())
            return;
        audioProcessor.setGainDecibels (static_cast<float> (volumeSlider.getMinimum()));
        updateFromProcessor();
    };
    waveform->onSeek = [this] (double seconds) { audioProcessor.seekToSeconds (seconds); };
    waveform->onAddMarker = [this] (double seconds) { audioProcessor.addMarker (seconds); updateFromProcessor(); };
    waveform->onClearMarkers = [this] { audioProcessor.clearMarkers(); updateFromProcessor(); };
    addAndMakeVisible (*waveform);

    volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    volumeSlider.setComponentID ("volume");
    volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setRange (-60.0, 6.0, 0.1);
    volumeSlider.setDoubleClickReturnValue (true, 0.0);
    volumeSlider.setTooltip (juce::String::fromUTF8 ("Output gain: −60 dB to +6 dB. Double-click to reset to 0 dB."));
    volumeSlider.setTitle ("Output volume in decibels");
    volumeSlider.onValueChange = [this]
    {
        audioProcessor.setGainDecibels (static_cast<float> (volumeSlider.getValue()));
        updateFromProcessor();
    };
    addAndMakeVisible (volumeSlider);

    setSize (editorWidth, editorHeight);
    setResizeLimits (editorWidth, editorHeight, editorWidth, editorHeight);
    updateFromProcessor();
    startTimerHz (30);
}

WaveformPlayerAudioProcessorEditor::~WaveformPlayerAudioProcessorEditor()
{
    stopTimer();
    tooltips.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void WaveformPlayerAudioProcessorEditor::configureButton (juce::TextButton& button,
                                                         const juce::String& text,
                                                         const juce::String& tooltip)
{
    button.setButtonText (text);
    button.setName (text);
    button.setDescription (tooltip);
    button.setTooltip (tooltip);
    button.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    addAndMakeVisible (button);
}

void WaveformPlayerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
    drawPanel (g, volumePanelBounds.toFloat());
    g.setColour (textSecondary);
    g.setFont (interfaceFont (interfaceTypeface, 11.0f));
    g.drawText ("v0.2", getWidth() - horizontalMargin - 43, 20, 43, headingHeight, juce::Justification::centred);
}

void WaveformPlayerAudioProcessorEditor::resized()
{
    const auto gap = getHeight() < 440 ? 12 : 16;
    auto bounds = getLocalBounds().reduced (horizontalMargin, 20);
    auto heading = bounds.removeFromTop (headingHeight);
    heading.removeFromRight (43);
    heading.removeFromRight (16);
    subtitleLabel.setBounds (heading.removeFromRight (270));
    heading.removeFromRight (16);
    titleLabel.setBounds (heading.withTrimmedLeft (8));
    bounds.removeFromTop (8);

    auto controls = bounds.removeFromBottom (36);
    const auto buttonWidth = (controls.getWidth() - 7 * 8) / 8;
    for (auto* button : { &openButton, &rewindButton, &playButton, &pauseButton, &stopButton, &forwardButton, &loopButton, &speedButton })
    {
        button->setBounds (controls.removeFromLeft (buttonWidth));
        controls.removeFromLeft (8);
    }
    bounds.removeFromBottom (gap);

    volumePanelBounds = bounds.removeFromBottom (52);
    const auto volumeBounds = volumePanelBounds.reduced (14, 8);
    const auto alignedVolumeButtonBounds = [&volumeBounds] (const juce::TextButton& target)
    {
        // Match width parity so integer bounds retain the target's exact centre.
        const auto width = 32 + target.getWidth() % 2;
        return juce::Rectangle<int> { target.getX() + (target.getWidth() - width) / 2,
                                      volumeBounds.getY(), width, volumeBounds.getHeight() };
    };
    minimumVolumeButton.setBounds (alignedVolumeButtonBounds (openButton));
    muteButton.setBounds (alignedVolumeButtonBounds (loopButton));
    const auto sliderLeft = minimumVolumeButton.getRight() + 8;
    volumeSlider.setBounds (sliderLeft, volumeBounds.getY(), muteButton.getX() - 8 - sliderLeft,
                            volumeBounds.getHeight());
    const auto valueLeft = muteButton.getRight() + 8;
    volumeValueLabel.setBounds (valueLeft, volumeBounds.getY(), volumeBounds.getRight() - valueLeft,
                                volumeBounds.getHeight());
    bounds.removeFromBottom (gap);

    waveform->setBounds (bounds);
}

void WaveformPlayerAudioProcessorEditor::timerCallback() { updateFromProcessor(); }

void WaveformPlayerAudioProcessorEditor::updateFromProcessor()
{
    const auto snapshot = audioProcessor.getPlaybackSnapshot();
    const auto available = snapshot.loaded && ! snapshot.loading && ! snapshot.choosingFile;
    const auto muted = audioProcessor.isMuted();
    openButton.setToggleState (snapshot.choosingFile, juce::dontSendNotification);
    openButton.setEnabled (! snapshot.loading && ! snapshot.choosingFile);
    speedButton.setEnabled (! snapshot.choosingFile);
    rewindButton.setEnabled (available && snapshot.positionSeconds > snapshot.rangeStartSeconds);
    playButton.setEnabled (available && ! snapshot.playing);
    pauseButton.setEnabled (available && snapshot.playing);
    stopButton.setEnabled (available && (snapshot.playing || snapshot.positionSeconds > snapshot.rangeStartSeconds));
    forwardButton.setEnabled (available && snapshot.positionSeconds < snapshot.rangeEndSeconds);
    loopButton.setEnabled (available);
    volumeSlider.setEnabled (available && ! muted);
    muteButton.setEnabled (available);
    minimumVolumeButton.setEnabled (available && ! muted);

    const auto speed = snapshot.playbackSpeed;
    const auto speedText = juce::String (speed) + "x";
    speedButton.setComponentID ("speed" + juce::String (speed));
    speedButton.setButtonText ("Speed " + speedText);
    speedButton.setName ("Playback speed: " + speedText);
    speedButton.setTooltip ("Playback speed: " + speedText + ". Click to cycle 1x, 2x, 4x, 8x.");
    speedButton.setDescription (speedButton.getTooltip());
    const auto looping = audioProcessor.isLooping();
    loopButton.setToggleState (looping, juce::dontSendNotification);
    loopButton.setButtonText (looping ? "Loop On" : "Loop Off");
    loopButton.setName (loopButton.getButtonText());
    const auto hasPair = snapshot.markerSamples[1] >= 0;
    loopButton.setTooltip (looping ? hasPair ? "Loop On: repeat the A/B range" : "Loop On: repeat the whole audio file"
                                  : hasPair ? "Loop Off: stop and return to A at B" : "Loop Off: stop and return to the start at the end");
    loopButton.setDescription (loopButton.getTooltip());
    muteButton.setToggleState (muted, juce::dontSendNotification);
    muteButton.setName (muted ? "Unmute output" : "Mute output");
    muteButton.setTitle (muteButton.getName());
    muteButton.setTooltip (muted ? "Output muted. Click to unmute without changing the volume setting."
                                : "Click to mute the output without changing the volume setting.");
    muteButton.setDescription (muteButton.getTooltip());
    const auto gain = audioProcessor.getGainDecibels();
    volumeSlider.setValue (gain, juce::dontSendNotification);
    volumeValueLabel.setText ((gain > 0.0f ? "+" : "") + juce::String (gain, 1) + " dB",
                              juce::dontSendNotification);
    volumeValueLabel.setColour (juce::Label::textColourId, muted ? textSecondary : textPrimary);
    waveform->update (snapshot);
}
