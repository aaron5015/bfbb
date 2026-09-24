# Nothing is minified (minifyEnabled is false), so this file is empty and
# exists because the release build type names it.
#
# If it is ever turned on, SDL's Java classes are reached from native code by
# name through JNI and cannot be renamed:
#   -keep class org.libsdl.app.** { *; }
