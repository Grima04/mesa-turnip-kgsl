Continuous Integration & Testing
================================


GitLab CI
---------

GitLab provides a convenient framework for running commands in response to git pushes.
We use it to test merge requests (MRs) before merging them (pre-merge testing),
as well as post-merge testing, for everything that hits ``master``
(this is necessary because we still allow commits to be pushed outside of MRs,
and even then the MR CI runs in the forked repository, which might have been
modified and thus is unreliable).

The CI runs a number of tests, from trivial build-testing to complex GPU rendering:

- Build testing for a number of build systems, configurations and platforms
- Sanity checks (``meson test`` & ``scons check``)
- Some drivers (softpipe, llvmpipe, freedreno and panfrost) are also tested
  using `VK-GL-CTS <https://github.com/KhronosGroup/VK-GL-CTS>`__

A typical run takes between 20 and 30 minutes, although it can go up very quickly
if the GitLab runners are overwhelmed, which happens sometimes. When it does happen,
not much can be done besides waiting it out, or cancel it.

Due to limited resources, we currently do not run the CI automatically
on every push; instead, we only run it automatically once the MR has
been assigned to ``Marge``, our merge bot.

If you're interested in the details, the main configuration file is ``.gitlab-ci.yml``,
and it references a number of other files in ``.gitlab-ci/``.

If the GitLab CI doesn't seem to be running on your fork (or MRs, as they run
in the context of your fork), you should check the "Settings" of your fork.
Under "CI / CD" → "General pipelines", make sure "Custom CI config path" is
empty (or set to the default ``.gitlab-ci.yml``), and that the
"Public pipelines" box is checked.

If you're having issues with the GitLab CI, your best bet is to ask
about it on ``#freedesktop`` on Freenode and tag `Daniel Stone
<https://gitlab.freedesktop.org/daniels>`__ (``daniels`` on IRC) or
`Eric Anholt <https://gitlab.freedesktop.org/anholt>`__ (``anholt`` on
IRC).


Intel CI
--------

The Intel CI is not yet integrated into the GitLab CI.
For now, special access must be manually given (file a issue in
`the Intel CI configuration repo <https://gitlab.freedesktop.org/Mesa_CI/mesa_jenkins>`__
if you think you or Mesa would benefit from you having access to the Intel CI).
Results can be seen on `mesa-ci.01.org <https://mesa-ci.01.org>`__
if you are *not* an Intel employee, but if you are you
can access a better interface on
`mesa-ci-results.jf.intel.com <http://mesa-ci-results.jf.intel.com>`__.

The Intel CI runs a much larger array of tests, on a number of generations
of Intel hardware and on multiple platforms (x11, wayland, drm & android),
with the purpose of detecting regressions.
Tests include
`Crucible <https://gitlab.freedesktop.org/mesa/crucible>`__,
`VK-GL-CTS <https://github.com/KhronosGroup/VK-GL-CTS>`__,
`dEQP <https://android.googlesource.com/platform/external/deqp>`__,
`Piglit <https://gitlab.freedesktop.org/mesa/piglit>`__,
`Skia <https://skia.googlesource.com/skia>`__,
`VkRunner <https://github.com/Igalia/vkrunner>`__,
`WebGL <https://github.com/KhronosGroup/WebGL>`__,
and a few other tools.
A typical run takes between 30 minutes and an hour.

If you're having issues with the Intel CI, your best bet is to ask about
it on ``#dri-devel`` on Freenode and tag `Clayton Craft
<https://gitlab.freedesktop.org/craftyguy>`__ (``craftyguy`` on IRC) or
`Nico Cortes <https://gitlab.freedesktop.org/ngcortes>`__ (``ngcortes``
on IRC).
