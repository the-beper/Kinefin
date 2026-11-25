# The build image plus what makes a dev run look like Plasma: the Breeze Qt style and the KDE platform theme (palette, fonts and icons from the desktop's kdeglobals). build/dev-run.sh builds it once; remove the image to rebuild it after a change of the build image
FROM localhost/kinefin-build:fedora45
USER root
RUN dnf install -y --setopt=install_weak_deps=False plasma-breeze plasma-integration && dnf clean all
