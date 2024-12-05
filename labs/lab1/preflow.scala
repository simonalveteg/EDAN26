import scala.util._
import java.util.Scanner
import java.io._
import akka.actor._
import akka.pattern.ask
import akka.util.Timeout
import scala.concurrent.{Await,ExecutionContext,Future,Promise}
import scala.concurrent.duration._
import scala.language.postfixOps
import scala.io._

case class Inflow(f: Int)
case class Debug(debug: Boolean)
case class Control(control:ActorRef)
case class Source(n: Int)
case class Push(incomingHeight: Int, index: Int, d: Int)
case class Nack(index: Int, d: Int)
case class Outflow(f:Int)

case object Print
case object Start
case object Excess
case object Maxflow
case object Sink
case object Hello
case object Ack

class Edge(var u: ActorRef, var v: ActorRef, var c: Int) {
	var	f = 0
}

class Node(val index: Int) extends Actor {
	var	excess = 0;				/* excess preflow. 						*/
	var	height = 0;				/* height. 							*/
	var	control:ActorRef = null		/* controller to report to when e is zero. 			*/
	var	source:Boolean	= false		/* true if we are the source.					*/
	var	sink:Boolean	= false		/* true if we are the sink.					*/
	var	edgeList: List[Edge] = Nil		/* adjacency list with edge objects shared with other nodes.	*/
	var	debug = false			/* to enable printing.						*/
  var k = 0 // How many edges we have tried to push to.
  var pendingRequests = 0 // how many push requests we have sent.
	
	def min(a:Int, b:Int) : Int = { if (a < b) a else b }

	def id: String = "@" + index;

	def other(a:Edge, u:ActorRef) : ActorRef = { if (u == a.u) a.v else a.u }

	def status: Unit = { if (debug) println(id + " e = " + excess + ", h = " + height) }

	def enter(func: String): Unit = { if (debug) { println(id + " enters " + func); status  } }
	def exit(func: String): Unit = { if (debug) { println(id + " exits " + func); status }  }

  def dbprint(string: String): Unit = { if (debug) { println(id + " " + string); status } }

	def relabel() : Unit = {

		enter("relabel")

		height += 1

		exit("relabel")
	}

  def push(e: Edge, index: Int) : Unit = {
    enter("push")
    var d = 0 // delta

    if (self == e.u) {
      d = min(excess, e.c - e.f)
      if (d != 0) {
        dbprint(s"Pushing ${d}")
        e.f += d
        e.v ! Push(height, index, d)
        pendingRequests += 1
        excess -= d
      }
    } else {
      d = min(excess, e.c + e.f)
      if (d != 0) {
        dbprint(s"Pushing ${d}")
        e.f -= d
        e.u ! Push(height, index, d)
        pendingRequests += 1
        excess -= d
      }
    }

    
    exit("push")
  }

  def discharge() : Unit = {
    enter(s"discharge. k = ${k}, edgeListSize = ${edgeList.size}")
    if (sink) return
    // loopa igenom edges, skicka maximal mängd, spara index i k
    if (k == edgeList.size) {
      if (pendingRequests > 0) return // we have shit to wait for
      k = 0
      if (!source) {
        relabel()
      }
    }
    for (index <- edgeList.indices) {
      if (excess > 0 && index >= k) {
        dbprint(s"Try push to edge ${index}")
        push(edgeList(index), index)
        k = index + 1
      }
    }
    exit("discharge")
  }

	def receive = {

  case Start => {
    enter("start")
    // Set excess to sum of edge capacity.
    // for (e <- edgeList) {
    //   excess += e.c
    // }
    // Push max flow on all edges.
    for (index <- edgeList.indices) {
      var e = edgeList(index)
      excess -= e.c
      if (self == e.u) {
        e.f += e.c
      } else {
        e.f -= e.c
      }
      e.v ! Push(height, index, e.c)
    }
    exit("start")
  }

	case Debug(debug: Boolean)	=> this.debug = debug

	case Print => status

	// case Excess => { sender ! Flow(excess) /* send our current excess preflow to actor that asked for it. */ }

	case edge:Edge => { this.edgeList = edge :: this.edgeList /* put this edge first in the adjacency-list. */ }

	case Control(control:ActorRef)	=> this.control = control

  case Push(incomingHeight: Int, index: Int, d: Int) => {
    // println(s"Received push request. Excess: ${excess}, Incoming: ${d}")
    // jämför höjd, svara med Ack eller Nack
    if (incomingHeight > height) {
      excess += d
      dbprint("Sending Ack")
      sender ! Ack
      if (source) {
        control ! Inflow(excess)
      }
      if (sink) {
        control ! Outflow(excess)
      } else {
        discharge()
      }
    } else {
      dbprint("Sending Nack")
      sender ! Nack(index, d)
    }
  }

	case Sink	=> { sink = true }

  case Ack => {
    dbprint("Received Ack")
    pendingRequests -= 1
    if (source) {
      control ! Inflow(excess)
    }
    discharge()
  }

  case Nack(index: Int, d: Int) => {
    dbprint("Received Nack")
    val e = edgeList(index)

    if (self == e.u) {
      e.f -= d
    } else {
      e.f += d
    }
    if (source) {
      control ! Inflow(excess)
    }

    // assert(math.abs(e.f) > e.c)

    pendingRequests -= 1
    excess += d
    discharge()
  }

	case Source(n:Int)	=> { height = n; source = true }

	case _		=> {
		println("" + index + " received an unknown message" + _) }

		assert(false)
	}

}


class Preflow extends Actor
{
	var	s	= 0;			/* index of source node.					*/
	var	t	= 0;			/* index of sink node.					*/
	var	n	= 0;			/* number of vertices in the graph.				*/
	var	edge:Array[Edge]	= null	/* edges in the graph.						*/
	var	node:Array[ActorRef]	= null	/* vertices in the graph.					*/
	var	ret:ActorRef 		= null	/* Actor to send result to.					*/
  var inflow = 0 // flow out from source
  var outflow = 0 // excess at sink
  
  def isDone() {
    // println(s"Is Done? Flow out: ${outflow}, Flow in: ${inflow}")
    if (math.abs(inflow) == outflow) {
      ret ! outflow
    }
  }

	def receive = {

	case node:Array[ActorRef]	=> {
		this.node = node
		n = node.size
		s = 0
		t = n-1
		for (u <- node)
			u ! Control(self)
	}

	case edge:Array[Edge] => this.edge = edge

  case Outflow(f:Int) => {
    outflow = f
    isDone()
  }

	case Inflow(f:Int) => {
    inflow = f
    isDone()
	}

	case Maxflow => {
		ret = sender
    node(s) ! Source(n)
    node(t) ! Sink
    node(s) ! Start
		// node(t) ! Excess	/* ask sink for its excess preflow (which certainly still is zero). */
	}
	}
}

object main extends App {
	implicit val t = Timeout(300 seconds);
	// implicit val t = Timeout(4 seconds);

	val	begin = System.currentTimeMillis()
	val system = ActorSystem("Main")
	val control = system.actorOf(Props[Preflow], name = "control")

	var	n = 0;
	var	m = 0;
	var	edge: Array[Edge] = null
	var	node: Array[ActorRef] = null

	val	s = new Scanner(System.in);

	n = s.nextInt
	m = s.nextInt

	/* next ignore c and p from 6railwayplanning */
	s.nextInt
	s.nextInt

	node = new Array[ActorRef](n)

	for (i <- 0 to n-1)
		node(i) = system.actorOf(Props(new Node(i)), name = "v" + i)

	edge = new Array[Edge](m)

	for (i <- 0 to m-1) {

		val u = s.nextInt
		val v = s.nextInt
		val c = s.nextInt

		edge(i) = new Edge(node(u), node(v), c)

		node(u) ! edge(i)
		node(v) ! edge(i)
	}

	control ! node
	control ! edge

	val flow = control ? Maxflow
	val f = Await.result(flow, t.duration)

	println("f = " + f)

	system.stop(control);
	system.terminate()

	val	end = System.currentTimeMillis()

	println("t = " + (end - begin) / 1000.0 + " s")
}
