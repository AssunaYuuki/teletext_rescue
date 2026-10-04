"""Сборка «Teletext Rescue.exe» (PyInstaller, папка с программой — без установки Python).

  python build_exe.py
Результат: dist/Teletext Rescue/Teletext Rescue.exe (+ папка _internal) и dist/Teletext Rescue.zip.
Скрипты из pages/ кладутся рядом с данными (_internal): программа запускает их через сам .exe
(teletext_gui.run_script), а шаблоны и .npy ищутся рядом с модулями.
"""
import glob, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAGES = os.path.join(HERE, 'pages')
NAME = 'Teletext Rescue'


def main():
    mods = sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(PAGES, '*.py')))
    sep = os.pathsep
    args = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--onedir', '--windowed',
            '--name', NAME, '--icon', os.path.join(HERE, 'icon.ico'),
            '--paths', PAGES, '--paths', os.path.join(PAGES, 'experiments'),
            '--distpath', os.path.join(HERE, 'dist'), '--workpath', os.path.join(HERE, 'build'),
            '--specpath', os.path.join(HERE, 'build')]
    for m in mods + ['mlse_fit', 'gui_parts']:
        args += ['--hidden-import', m]
    for pat in ('*.py', '*.npy', '*.html', '*.js'):
        for f in glob.glob(os.path.join(PAGES, pat)):
            args += ['--add-data', f + sep + '.']
    args += ['--add-data', os.path.join(PAGES, 'experiments', 'mlse_fit.py') + sep + 'experiments']
    args += ['--add-data', os.path.join(HERE, 'icon.ico') + sep + '.']
    try:
        import pyopencl  # noqa: F401  — видеокарта (необязательно)
        args += ['--collect-all', 'pyopencl']
    except ImportError:
        pass
    # pyopencl тянет необязательные тяжёлые библиотеки, программе они не нужны
    for m in ('PyQt5', 'PyQt6', 'PySide2', 'PySide6', 'shiboken6', 'matplotlib', 'scipy', 'OpenGL',
              'OpenGL_accelerate', 'IPython', 'jupyter', 'notebook', 'pandas', 'pytest', 'sympy'):
        args += ['--exclude-module', m]
    args.append(os.path.join(HERE, 'teletext_gui.py'))
    subprocess.run(args, check=True)
    out = os.path.join(HERE, 'dist', NAME)
    for f in ('README.txt', 'LICENSE'):
        shutil.copy(os.path.join(HERE, f), out)
    z = shutil.make_archive(os.path.join(HERE, 'dist', NAME), 'zip', os.path.join(HERE, 'dist'), NAME)
    print('built:', os.path.join(out, NAME + '.exe'))
    print('zip:  ', z)


if __name__ == '__main__':
    main()
