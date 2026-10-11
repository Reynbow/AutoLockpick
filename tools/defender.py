"""Windows Defender check of a release zip, and its Microsoft false-positive submission sheet, through the
defender_check.py that the mods share (next to the project folders). tools/package.ps1 runs it on every new zip:

    python tools/defender.py release/AutoLockpick-<version>.zip

and `python tools/defender.py --scan` re-checks every release zip without building anything (e.g. a day or two
after submitting). The scan only reports: it never quarantines anything.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(ROOT))  # defender_check.py is shared by the mods
try:
    import defender_check
except ImportError:
    defender_check = None

# No apostrophes in the text: the submission form stores them escaped over and over.
MOD = dict(name='AutoLockpick', github='https://github.com/Reynbow/AutoLockpick',
           game='Hellraiser: Revival',
           loaded='The game loads it as dsound.dll from its own folder, and it forwards every DirectSound function to '
                  'the Windows copy.',
           does='plays the lockpicking puzzle of the game for the player through the input functions of that puzzle, '
                'hooks one drawing function of the game in memory to run once per frame, and reads its hotkey only '
                'while the game window is in front')


def main():
    if not defender_check:
        sys.exit('Defender check skipped: ..\\defender_check.py not found')
    if '--scan' in sys.argv:
        defender_check.scan_all(MOD, os.path.join(ROOT, 'release'))
    elif len(sys.argv) == 2:
        defender_check.after_package(MOD, os.path.abspath(sys.argv[1]))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
