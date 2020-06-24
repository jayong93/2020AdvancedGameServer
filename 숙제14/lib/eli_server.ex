defmodule EliServer do
  @moduledoc """
  Documentation for EliServer.
  """

  @doc """
  Hello world.

  ## Examples

      iex> EliServer.hello()
      :world

  """
  def hello do
    :world
  end

  def broad_pos_msg([], _, _, _, _) do
    nil
  end

  def broad_pos_msg([head|tail], cid, x, y, pid) do
    send elem(head, 1), {:pos, cid, x, y, pid}
    broad_pos_msg(tail, cid, x, y, pid)
  end

  def sector(id, clients) do
    receive do
      {:sIN, cid, pid} ->
        clients = Map.put(clients, cid, pid)
        sector(id, clients)
      {:sOut, cid} ->
        clients = Map.delete(clients, cid)
        sector(id, clients)
      {:sbroad, cid, x, y, pid} ->
        EliServer.broad_pos_msg(Map.to_list(clients), cid, x, y, pid)
        sector(id, clients)
    end
  end

  def send_loginOK_packet(pid, id) do
    pk_size = 6
    pk_type = 1
    send(pid, {:send, <<pk_size::little-integer-size(8), pk_type::little-integer-size(8),
    id::little-integer-size(32)>>})
  end

  def send_put_player_packet(pid, id, x, y) do
    pk_size = 10
    pk_type = 2
    send(pid, {:send, <<pk_size::little-integer-size(8), pk_type::little-integer-size(8),
                          id::little-integer-size(32),
                          x::little-integer-signed-size(16), y::little-integer-signed-size(16)>>})
  end

  def send_remove_player_packet(pid, id) do
    pk_size = 6
    pk_type = 3
    send(pid, {:send, <<pk_size::little-integer-size(8), pk_type::little-integer-size(8),
                          id::little-integer-size(32)>>})
  end

  def send_pos_packet(pid, id, x, y) do
    pk_size = 10
    pk_type = 4
    send(pid, {:send, <<pk_size::little-integer-size(8), pk_type::little-integer-size(8),
                          id::little-integer-size(32),
                          x::little-integer-signed-size(16), y::little-integer-signed-size(16)>>})
  end


  def recv_packet(sock, pid, pk_size) do
    case :gen_tcp.recv(sock, pk_size) do
      {:ok, b} ->
        <<pk_type::little-integer-size(8)>> = b
        send pid, {:packet, pk_type}
        recv_packet(sock, pid)
      {:error, reason} ->
        IO.inspect(reason, label: "error at recv, with pk_size(#{pk_size})")
        send pid, {:packet, -1}
        :gen_tcp.close(sock)
    end
  end

  def recv_packet(sock, pid) do
    case :gen_tcp.recv(sock, 1) do
      {:ok, b} ->
        <<pk_size::little-integer-size(8)>> = b
        recv_packet(sock, pid, pk_size - 1)
      {:error, reason} ->
        IO.inspect(reason, label: "error at recv")
        send pid, {:packet, -1}
        :gen_tcp.close(sock)
    end
  end

  @spec get_view_sectors(integer, integer) :: MapSet.t(any)
  def get_view_sectors(x, y) do
    map_set = MapSet.new()

    x1 = div((x - 4) + abs(x - 4), 2)
    x2 = (1 - div(x + 4, 300))*(x + 4) + (div(x + 4, 300))*(300 - 1)
    y1 = div((y - 4) + abs(y - 4), 2)
    y2 =  (1 - div(y + 4, 300))*(y + 4) + (div(y + 4, 300))*(300 - 1)

    map_set = MapSet.put(map_set, div(y1,20)*15 + div(x1,20))
    map_set = MapSet.put(map_set, div(y1,20)*15 + div(x2,20))
    map_set = MapSet.put(map_set, div(y2,20)*15 + div(x1,20))
    map_set = MapSet.put(map_set, div(y2,20)*15 + div(x2,20))

    map_set
  end

  def send_sbroadMsg([head|tail], id, x, y, pid, sectors) do
    send elem(sectors, head), {:sbroad, id, x, y, pid}
    send_sbroadMsg(tail, id, x, y, pid, sectors)
  end

  def send_sbroadMsg([], _, _, _, _, _) do
    nil
  end

  def in_my_view?(mx, my, tx, ty) do
    case ((mx - 4 <= tx and tx <= mx + 4) and (my - 4 <= ty and ty <= my + 4)) do
      x ->
        x
    end
  end

  def loop_client(client, x, y, id, sectors, viewList, viewSectors) do
    receive do
      {:packet, pkType} ->
        # code
        {nx, ny} =
        case pkType do
          1 ->
            {x, div((y - 1) + abs(y - 1), 2)}
          2 ->
            {x, (1 - div(y + 1, 300))*(y + 1) + (div(y + 1, 300))*(300 - 1)}
          3 ->
            {div((x - 1) + abs(x - 1), 2), y}
          4 ->
            {(1 - div(x + 1, 300))*(x + 1) + (div(x + 1, 300))*(300 - 1), y}
          -1 ->
            IO.puts ("client out")
            send elem(sectors, div(y, 20)*15 + div(x, 20)), {:sOut, id}
            send_sbroadMsg(MapSet.to_list(viewSectors), id, -1, -1, self(), sectors)
            exit(:shutdown)
          _ ->
            IO.puts ("Unknown Packet Type Error")
            {x, y}
        end


        case div(x, 20) != div(nx, 20) or div(y, 20) != div(ny, 20) do
           true ->
            send elem(sectors, div(ny, 20)*15 + div(nx, 20)), {:sIN, id, self()}
            send elem(sectors, div(y, 20)*15 + div(x, 20)), {:sOut, id}
            _ ->
              _ = nil
        end
        new_viewSectors = EliServer.get_view_sectors(nx, ny)
        send_sbroadMsg(MapSet.to_list(MapSet.union(viewSectors, new_viewSectors)), id, nx, ny, self(), sectors)
        EliServer.send_pos_packet(client, id, nx, ny)
        loop_client(client, nx, ny, id, sectors, viewList, new_viewSectors)

      {:hello, tid, tx, ty} ->
          case MapSet.member?(viewList, tid) do
            false ->
              viewList = MapSet.put(viewList, tid)
              EliServer.send_put_player_packet(client, tid, tx, ty)
              loop_client(client, x, y, id, sectors, viewList, viewSectors)
            _ ->
              loop_client(client, x, y, id, sectors, viewList, viewSectors)
          end

      {:bye, tid} ->
          case MapSet.member?(viewList, tid) do
            true ->
              viewList = MapSet.delete(viewList, tid)
              EliServer.send_remove_player_packet(client, tid)
              loop_client(client, x, y, id, sectors, viewList, viewSectors)
            _ ->
              loop_client(client, x, y, id, sectors, viewList, viewSectors)
          end
      {:pos, tid, tx, ty, pid} ->
          case tid == id do
            true ->  loop_client(client, x, y, id, sectors, viewList, viewSectors)
            false ->
              case MapSet.member?(viewList, tid) do
                true ->
                  case {tx, ty} do
                    {-1, -1} ->
                      viewList = MapSet.delete(viewList, tid)
                      EliServer.send_remove_player_packet(client, tid)
                      loop_client(client, x, y, id, sectors, viewList, viewSectors)
                    _ ->
                      in_view = EliServer.in_my_view?(x, y, tx, ty)
                      case in_view do
                        true ->
                          EliServer.send_pos_packet(client, tid, tx, ty)
                          loop_client(client, x, y, id, sectors, viewList, viewSectors)
                        false ->
                          viewList = MapSet.delete(viewList, tid)
                          EliServer.send_remove_player_packet(client, tid)
                          send pid, {:bye, id}
                          loop_client(client, x, y, id, sectors, viewList, viewSectors)
                      end
                  end
                false ->
                  case {tx, ty} do
                    {-1, -1} ->
                      loop_client(client, x, y, id, sectors, viewList, viewSectors)
                    _ ->
                      in_view = EliServer.in_my_view?(x, y, tx, ty)
                      case in_view do
                        true ->
                          viewList = MapSet.put(viewList, tid)
                          EliServer.send_put_player_packet(client, tid, tx, ty)
                          send pid, {:hello, id, x, y}
                          loop_client(client, x, y, id, sectors, viewList, viewSectors)
                        false ->
                          loop_client(client, x, y, id, sectors, viewList, viewSectors)
                      end
                  end
              end
          end

          loop_client(client, x, y, id, sectors, viewList, viewSectors)
    end
  end

  def handle_client(sender, sx, sy, id, sectors) do
    x = sx
    y = sy

    EliServer.send_loginOK_packet(sender, id)

    col = div(sx, 20)
    row = div(sy, 20)

    send elem(sectors, row*15 + col), {:sIN, id, self()}
    viewSectors = EliServer.get_view_sectors(x, y)
    send_sbroadMsg(MapSet.to_list(viewSectors), id, x, y, self(), sectors)

    EliServer.send_pos_packet(sender, id, x, y)

    viewList = MapSet.new()
    EliServer.loop_client(sender, x, y, id, sectors, viewList, viewSectors)
  end

  def run_sectors_slave(n, sectors, _) when n == 15*15 do
    sectors
  end

  def run_sectors_slave(n, sectors, slave_node) do
    # spawn_link node
    pid = Node.spawn_link slave_node, fn -> sector(n, Map.new()) end
    run_sectors(n + 1, Tuple.append(sectors, pid), slave_node)
  end

  def run_sectors(n, sectors, _) when n == 15*15 do
    sectors
  end

  def run_sectors(n, sectors, slave_node) do
    #pid = spawn_link(EliServer, :sector, [n])
    pid = spawn_link fn -> sector(n, Map.new()) end
    run_sectors_slave(n + 1, Tuple.append(sectors, pid), slave_node)
  end

  defp loop_sender(conn) do
    receive do
      {:send, packet} ->
        :gen_tcp.send(conn, packet)
        loop_sender(conn)
    end
  end

  defp loop_acceptor(slave_node, socket, nextID, sectors) do
    {:ok, client} = :gen_tcp.accept(socket)
    #pid = Task.start(fn -> handle_client(client) end)
    #pid = spawn(EliServer, :handle_client, [client])

    sender = spawn fn -> loop_sender(client) end

    x = :rand.uniform(300 - 1)
    y = case Integer.mod(nextID, 2) do
          0 -> :rand.uniform(150 - 1)
          _ -> :rand.uniform(150 - 1) + 150
        end
    #x = :rand.uniform(40 - 1)
    #y = :rand.uniform(40 - 1)

    pid = case Integer.mod(nextID, 2) do
            0 -> Node.spawn slave_node, fn -> handle_client(sender, x, y, nextID, sectors) end 
            _ -> spawn fn -> handle_client(sender, x, y, nextID, sectors) end
          end
    
    spawn fn -> recv_packet(client, pid) end
    # :ok = :gen_tcp.controlling_process(client, pid)

    loop_acceptor(slave_node, socket, nextID + 1, sectors)
  end

  def accept(port, slave_node) do
    sectors = EliServer.run_sectors(0, {}, slave_node)
    IO.puts("Launching server.....")

    {:ok, socket} = :gen_tcp.listen(port,  [:binary, {:packet, 0}, {:active, false}])
    IO.puts("Accepting connections on port #{port}")

    loop_acceptor(slave_node, socket, 0, sectors)
  end

  def run do
    [server_id|[]] = System.argv
    case server_id do
      "0" ->
        Node.start :"master@127.0.0.1"
        case Node.connect :"slave@127.0.0.1" do
          true -> EliServer.accept(3500, :"slave@127.0.0.1")
          false -> IO.puts "Slave server has not been launched"
        end
      _ -> 
        Node.start :"slave@127.0.0.1"
        receive do
          _ -> nil
        end
    end
  end

  def start(_type, _args) do
    EliServer.run
  end

end
