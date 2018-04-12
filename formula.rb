class Afsctool < Formula
  desc "Utility for manipulating HFS+ compressed files"
  homepage "https://github.com/RJVB/afsctool"
  url "https://github.com/RJVB/afsctool/archive/1.6.9.zip"
  version "1.6.9"
  sha256 "0e2c9ddf1a4b63ab50604bed5d5e124cc2c6f27e850c700bc2cec308ecd4267c"
  revision 1

  bottle do
    cellar :any_skip_relocation
    sha256 "8f1d0020beff7bcfd80530f69d29ca7f6bf053623c84a9957ef7dd5cd4fa3536" => :high_sierra
    sha256 "7f11c9c16fb0f5f148fb80f1888cfa1053296e8f552b11cc196a6c3fcf0afade" => :sierra
    sha256 "0751efedf08e3d0c4efed48861aaddf150bec2fdabc7099306f576a8c63c4971" => :el_capitan
    sha256 "a05524c78e0153712e5a9d1a73fe70ed2e5f0e5e20a91ae38fedf9115d2e87a4" => :yosemite
  end

  depends_on "cmake" => :build
  depends_on "google-sparsehash" => :build

  def install
    system "cmake", ".", *std_cmake_args
    system "make", "install"
  end

  test do
    path = testpath/"foo"
    path.write "some text here."
    system "#{bin}/afsctool", "-c", path
    system "#{bin}/afsctool", "-v", path
  end
end
