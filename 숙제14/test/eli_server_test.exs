defmodule EliServerTest do
  use ExUnit.Case
  doctest EliServer

  test "greets the world" do
    assert EliServer.hello() == :world
  end
end
