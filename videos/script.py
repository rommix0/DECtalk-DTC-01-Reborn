"""The explainer video's narration (videos/DESIGN.md, PLAN.md Task 4): Perfect Paul's lines, scene by scene.

`say` is what Paul is sent: plain ASCII DECtalk text, spelled so v1.8 says it right (letters spaced out, numbers in
words where the digits would be read wrongly); `[...]` is phonemic text. `show` is the caption when it differs.
Every fact here comes from REFERENCE.md, dsp/dsp_synth_topology, PITCH_SYSTEM.md, FORMANT_SYSTEM.md or
SMOOTHING_SYSTEM.md.
"""
from dataclasses import dataclass, field

from data import VOICE_LINES, VOICE_NAMES
from make_video import GREETING


@dataclass
class Line:
    say: str                    # what Paul is sent: ASCII DECtalk text, phonemic [..] allowed
    show: str | None = None     # the caption when it differs from say (real spellings: F1, TMS32010, µs)
    pause: float = 0.6          # seconds of silence after the line
    boot: bool = False          # rendered with no lead-in (the power-up greeting itself)


@dataclass
class Scene:
    id: str
    lead: float                 # seconds before the first line
    tail: float                 # seconds after the last line; with the next scene's lead at most 3 s (the user)
    lines: list[Line] = field(default_factory=list)


CLASSES = {"cold_open": "ColdOpen", "big_picture": "BigPicture", "text_to_phonemes": "TextToPhonemes",
           "timing_pitch": "TimingPitch", "formants": "Formants", "frame": "Frame", "dsp_rhythm": "DspRhythm",
           "dsp_glottal": "DspGlottal", "dsp_noise": "DspNoise", "dsp_cascade": "DspCascade",
           "dsp_parallel": "DspParallel", "dsp_dac": "DspDac", "voices": "DspVoices", "outro": "Outro"}


def scene_class(scene_id):
    return CLASSES[scene_id]


def voice_line(cmd):
    return Line("[:%s] %s" % (cmd, VOICE_LINES[cmd]), "%s: %s" % (VOICE_NAMES[cmd], VOICE_LINES[cmd]), pause=0.4)


# Daisy Bell (1892): notes 1-37 are C2-C5 (REFERENCE s15.14), so 20 = G3, 25 = C4, 29 = E4, 32 = G4
DAISY = ("[d'ey<900,32>ziy<900,29> d'ey<900,25>ziy<900,20> g'ih<300,22>v m'iy<300,24> y'uh<300,25>r "
         "'ae<600,22>nsrr<300,25> d'uw<1200,20>]")

SCENES = [
    Scene("cold_open", 2.5, 1.2, [
        Line(GREETING, boot=True, pause=1.5),
        Line("Hello. I'm Perfect Paul, the default voice of the first Deck talk, the D T C zero one.",
             "Hello. I'm Perfect Paul, the default voice of the first DECtalk, the DTC-01."),
        Line("Everything you hear in this video, I am saying myself, rebuilt in C from my original firmware."),
        Line("Let me show you how I turn typed text into speech."),
    ]),
    Scene("big_picture", 1.0, 1.2, [
        Line("Inside me are two processors."),
        Line("A Motorola 68000 reads the text, and works out what to say and how to say it."),
        Line("The 68000 is the planner. The second chip is the voice."),
        Line("Every six point four milliseconds, it sends a frame of numbers to the second chip.",
             "Every 6.4 ms, it sends a frame of numbers to the second chip."),
        Line("That chip is a Texas Instruments T M S three twenty ten, a digital signal processor.",
             "That chip is a Texas Instruments TMS32010, a digital signal processor."),
        Line("It turns each frame into sixty four sound samples, one every hundred microseconds.",
             "It turns each frame into 64 sound samples, one every 100 µs."),
        Line("Let's follow one sentence all the way through. Hello, I'm Perfect Paul.", pause=1.0),
    ]),
    Scene("text_to_phonemes", 1.0, 1.2, [
        Line("First, the 68000 turns letters into phonemes, the sounds of English."),
        Line("Each word is looked up in a dictionary built into my rom.",
             "Each word is looked up in a dictionary built into my ROM."),
        Line("Hello, I'm, and perfect are in it, so their sounds come straight from the table."),
        Line("Paul is not, so it goes to my letter to sound rules.",
             "Paul is not, so it goes to my letter-to-sound rules."),
        Line("There are three hundred seventy three of them, descended from rules Hunnicutt published in 1976.",
             "There are 373 of them, descended from rules Hunnicutt published in 1976."),
        Line("Every rule is tried in order, at every position in the word."),
        Line("They run in sections: suffixes, consonants, prefixes, letter pairs, vowels, stress, and a final "
             "cleanup."),
        Line("In a name like Klatt, one rule turns the letter A into the vowel of cat, and a later one marks the "
             "stress.", "In a name like Klatt, one rule turns the letter A into the vowel of \"cat\"; a later one "
             "marks the stress."),
        Line("What comes out is a string of phonemes, with stress and boundaries marked."),
    ]),
    Scene("timing_pitch", 1.0, 1.2, [
        Line("Next, every phoneme gets a duration."),
        Line("It starts from an inherent length, and rules for stress, phrase position and neighbors stretch or "
             "shorten it.", "It starts from an inherent length; rules for stress, phrase position and neighbors "
             "stretch or shorten it."),
        Line("Durations are counted in frames of six point four milliseconds.",
             "Durations are counted in frames of 6.4 ms."),
        Line("Then comes the pitch. I use Dennis Klatt's hat pattern."),
        Line("The pitch steps up at the first stressed syllable, stays up like the top of a hat, and falls at the "
             "last one.", "The pitch steps up at the first stressed syllable, stays up like a hat's top, and "
             "falls at the last one."),
        Line("Stressed syllables add short bumps, over a baseline that slowly falls."),
        Line("A comma gets a small rise to say there is more, and a period gets a final fall."),
        Line("The sum is smoothed every frame, and becomes the pitch period sent to the D S P.",
             "The sum is smoothed every frame, and becomes the pitch period sent to the DSP."),
    ]),
    Scene("formants", 1.0, 1.2, [
        Line("Now the color of each sound: its formants."),
        Line("Formants are resonances of the vocal tract. The first three decide which vowel you hear."),
        Line("Every phoneme has a target for each formant, from a table for male voices or one for female voices."),
        Line("Each target is pulled fifteen percent toward its neighbors.",
             "Each target is pulled 15% toward its neighbors."),
        Line("Vowels like the I in I'm glide between points in straight lines, instead of holding still.",
             "Vowels like the I in \"I'm\" glide between points in straight lines, instead of holding still."),
        Line("At every boundary, a forward ramp and a backward ramp join one phoneme to the next, so nothing jumps."),
        Line("Next to a consonant, the ramp starts near its locus, the frequency that place in the mouth points to."),
        Line("Every frame, the 68000 reads all of these tracks at the current moment."),
    ]),
    Scene("frame", 1.0, 1.2, [
        Line("Those values become one frame of nineteen words."),
        Line("A header, then the pitch period, three formants, the nasal zero, and three bandwidths."),
        Line("Then the voicing and aspiration levels, five parallel gains, the bypass, the tilt, and a trailer."),
        Line("Frequencies go in hertz, and the levels in decibels. The D S P does the rest of the math.",
             "Frequencies go in hertz, and the levels in decibels. The DSP does the rest of the math."),
        Line("The frame goes to the D S P, and the 68000 starts on the next one.",
             "The frame goes to the DSP, and the 68000 starts on the next one."),
    ]),
    Scene("dsp_rhythm", 1.0, 1.2, [
        Line("Now we are inside the D S P, where the sound is made.",
             "Now we're inside the DSP, where the sound is made."),
        Line("It makes one sample every hundred microseconds, each time the converter asks for one.",
             "It makes one sample every 100 µs, each time the converter asks for one."),
        Line("After sixty four samples, it looks for the next frame.",
             "After 64 samples, it looks for the next frame."),
        Line("If none comes for three frame times, it stops and waits. The D S P sets the pace.",
             "If none comes for three frame times, it stops and waits. The DSP sets the pace."),
    ]),
    Scene("dsp_glottal", 1.0, 1.2, [
        Line("Voiced sounds begin at the glottis, the vocal folds."),
        Line("The D S P builds each glottal pulse from a table taken from Klatt's synthesizer program.",
             "The DSP builds each glottal pulse from a table taken from Klatt's synthesizer program."),
        Line("The folds open, the pulse grows, then they close and stay closed until the next period."),
        Line("The period is forty thousand divided by the pitch, counted at four times the sample rate.",
             "The period is 40,000 ÷ F0, counted at four times the sample rate."),
        Line("A low pass filter brings those four steps back down to one sample.",
             "A low-pass filter brings those four steps back down to one sample."),
        Line("A tilt filter softens the highs, and breath noise can be mixed into the open phase."),
        Line("Every other period is stretched or shortened a little, a skew that adds roughness to the voice."),
        Line("Last, the voicing amplitude scales the pulse, turned from decibels into a gain by a table in "
             "the D S P.", "Last, the voicing amplitude scales the pulse, turned from dB into a gain by a table "
             "in the DSP."),
    ]),
    Scene("dsp_noise", 1.0, 1.2, [
        Line("Hisses and bursts need noise."),
        Line("A sixteen bit shift register with feedback makes a new pseudo random number every sample.",
             "A 16-bit shift register with feedback makes a new pseudo-random number every sample."),
        Line("A filter takes the edge off, and in voiced sounds it is halved for part of every period."),
        Line("Aspiration noise joins the voicing at the input of the cascade."),
        Line("Its seed goes back to the same value every time the voice changes."),
    ]),
    Scene("dsp_cascade", 1.0, 1.2, [
        Line("Now the vocal tract itself. It is a cascade: seven filters in a row."),
        Line("Each resonator is one line of arithmetic."),
        Line("The new output is A times the input, plus B times the last output, plus C times the one before.",
             "The new output is A × the input, plus B × the last output, plus C × the one before."),
        Line("A, B and C come from the formant's frequency and bandwidth."),
        Line("It all runs in sixteen bit fixed point, with twelve bits after the binary point.",
             "It all runs in 16-bit fixed point, with 12 bits after the binary point."),
        Line("The nasal zero and the nasal pole come first. They shape nasals like M and N."),
        Line("Then formants five and four, which follow the speaker's head size.",
             "Then formants F5 and F4, which follow the speaker's head size."),
        Line("Then three, two, and one. Watch the spectrum: each resonator adds a peak.",
             "Then F3, F2 and F1. Watch the spectrum: each resonator adds a peak."),
        Line("The buzz from the glottis comes out as a vowel."),
        Line("The result is doubled, and goes on to the sum."),
    ]),
    Scene("dsp_parallel", 1.0, 1.2, [
        Line("Some sounds are not shaped that way. Fricatives like S and F are noise, filtered up high."),
        Line("So a second branch runs on the noise alone: formants two to six side by side, and a bypass.",
             "So a second branch runs on the noise alone: F2 to F6 side by side, and a bypass."),
        Line("Their outputs are added with alternating signs, and the frame sets every gain."),
        Line("Formant six is fixed at reset. Four and five come from the speaker, two and three from the frame.",
             "F6 is fixed at reset. F4 and F5 come from the speaker, F2 and F3 from the frame."),
        Line("In perfect, you hear it in the F, and in the bursts of the K and the T."),
    ]),
    Scene("dsp_dac", 1.0, 1.2, [
        Line("The two branches are added into one number."),
        Line("Its top twelve bits go to the converter, ten thousand times a second.",
             "Its top 12 bits go to the converter, 10,000 times a second."),
        Line("The converter keeps a queue of sixteen samples.", "The converter keeps a queue of 16 samples."),
        Line("A second kind of frame, twenty four words long, sets the voice whenever it changes.",
             "A second kind of frame, 24 words long, sets the voice whenever it changes."),
        Line("And that is how a buzz and a hiss become my voice.", pause=1.0),
    ]),
    Scene("voices", 1.0, 1.2, [
        Line("That speaker frame is all it takes to change who is talking."),
        *(voice_line(c) for c in ("nb", "nh", "nf", "nk", "nr", "nu")),
        Line("Doctor Dennis shares Frank's settings in this version, and Variable Val starts out as me."),
    ]),
    Scene("outro", 1.0, 2.5, [
        Line("From letters to rules, from targets to tracks, from frames to samples."),
        Line("Dennis Klatt designed this synthesizer. The D S P runs his cascade and parallel layout.",
             "Dennis Klatt designed this synthesizer. The DSP runs his cascade and parallel layout."),
        Line("Every sound in this video came from that chain, rebuilt in C from my original rom.",
             "Every sound in this video came from that chain, rebuilt in C from my original ROM."),
        Line("Thank you for listening. Before you go, a song.", pause=0.8),
        Line(DAISY, "♪ Daisy, Daisy, give me your answer, do ♪", pause=1.0),
    ]),
]
