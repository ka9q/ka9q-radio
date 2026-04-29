# See gbp(1) provided by git-buildpackage package for an explanation of each
# option or browse https://manpages.debian.org/unstable/git-buildpackage/gbp.1.en.html

[DEFAULT]
# DEP-14 format. Other naming convention are available,
# see DEP-14 for more details.
debian-branch = debian/latest
upstream-branch = upstream/latest

# Enable pristine-tar to exactly reproduce orig tarballs
pristine-tar = True

# The Debian packaging git repository may also host actual upstream tags and
# branches, typically named 'main' or 'master'. Configure the upstream tag
# format below, so that `gbp import-orig` will run correctly, and link tarball
# import branch (`upstream/latest`) with the equivalent upstream release tag,
# showing a complete audit trail of what upstream released and what was imported
# into Debian.
#upstream-vcs-tag = %(version%~%.)s

# If upstream publishes tarball signatures, git-buildpackage will by default
# import and use the them. Change this to 'on' to make 'gbp import-orig' abort
# if the signature is not found or is not valid.
#upstream-signatures = on

# Ensure the Debian maintainer signs git tags automatically.
#sign-tags = True

# Ease dropping / adding patches.
#patch-numbers = False

# Group debian/changelog entries with the same "[ Author ]" instead of making
# multiple ones for the same author.
#multimaint-merge = True

# Automatically open a new changelog entry about the new upstream release, but
# do not commit it, as the 'gbp dch' still needs to run and list all commits
# based on when the debian/changelog last was updated in a git commit.
#postimport = dch -v %(version)s "New upstream release"

# Ensure a human always reviews all the debian/changelog entries.
#spawn-editor = always

# No need to confirm package name or version at any time, git-buildpackage
# always gets it right.
#interactive = False

# Ensure we always target Debian on Debian branches.
#dch-opt = --vendor=debian

# If this package ever needs to be maintained for Ubuntu, remember to override
# the branch, tag and commit messages.
#debian-branch = ubuntu/24.04-noble
#debian-tag = ubuntu/%(version)s
#debian-tag-msg = %(pkg)s Ubuntu release %(version)s
#dch-opt = --vendor=ubuntu
