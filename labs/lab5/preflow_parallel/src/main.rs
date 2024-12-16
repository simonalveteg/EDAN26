#[macro_use] extern crate text_io;

use std::sync::{Mutex, Arc};
use std::collections::LinkedList;
use std::cmp;
use std::thread;
use std::collections::VecDeque;



struct Node {
	i:	usize,			/* index of itself for debugging.	*/
	e:	i32,			/* excess preflow.			*/
	h:	i32,			/* height.				*/
}

struct Edge {
        u:      usize,  
        v:      usize,
        f:      i32,
        c:      i32,
}

impl Node {
	fn new(ii:usize) -> Node {
		Node { i: ii, e: 0, h: 0 }
	}
}

impl Edge {
	fn new(uu:usize, vv:usize, cc:i32) -> Edge {
			Edge { u: uu, v: vv, f: 0, c: cc }      
	}
}


fn relabel(u:&mut Node, excess_list:&mut Arc<Mutex<VecDeque<usize>>>) {
	u.h += 1;
	//println!("Increasing height {}: {}", u.i, u.h);
	excess_list.lock().unwrap().push_back(u.i);
}

fn push(u:&mut Node, v:&mut Node, e:&mut Edge, excess_list:&mut Arc<Mutex<VecDeque<usize>>>) {

	let d = if u.i == e.u {
		let d = cmp::min(u.e, e.c - e.f);
		e.f += d;
		d
	} else {
		let d = cmp::min(u.e, e.c + e.f);
		e.f -= d;
		d
	};

	//println!("Push {} from {} to {}", d, u.i, v.i);

	u.e -= d;
	v.e += d;
	
	// The following are always true.
    assert!(d >= 0);
    assert!(u.e >= 0);
    assert!(e.f.abs() <= e.c);

	if u.e > 0 {
		excess_list.lock().unwrap().push_back(u.i);
	}

	if v.e == d {
		excess_list.lock().unwrap().push_back(v.i);
	}
}

fn discharge(
	u: usize,
    node: &mut Vec<Arc<Mutex<Node>>>,
    edge: &mut Vec<Arc<Mutex<Edge>>>,
    adj: &Vec<LinkedList<usize>>,
    excess: &mut Arc<Mutex<VecDeque<usize>>>
) {
	let mut b: i32;

	if u == 0 || u == node.len() - 1 {
		return;
	}

	let mut from_i = u;
	let mut from;
	for &e in adj[u].iter() {
		let mut edge = edge[e].lock().unwrap();
		let to_i;
		let mut to;

		if edge.u == u {
			to_i = edge.v;
			b = 1;
		} else {
			to_i = edge.u;
			from_i = edge.v;
			b = -1;
		}
		{
			if from_i < to_i {
				from = node[from_i].lock().unwrap();
				to = node[to_i].lock().unwrap();
			}else{
				to = node[to_i].lock().unwrap();
				from = node[from_i].lock().unwrap();
			}
			
			//println!("Node {}: Excess {}, Height {} to Node {}: Height {}", from.i, from.e, from.h, to.i, to.h);
			if from.h > to.h && b * edge.f < edge.c {
				push(&mut from, &mut to, &mut edge, excess);
				return;
			}
		}
		drop(to);
		drop(from);
	}
	from = node[u].lock().unwrap();
	relabel(&mut from, excess);
}

fn main() {

	let n: usize = read!();		/* n nodes.						*/
	let m: usize = read!();		/* m edges.						*/
	let _c: usize = read!();	/* underscore avoids warning about an unused variable.	*/
	let _p: usize = read!();	/* c and p are in the input from 6railwayplanning.	*/
	let num_threads = 8;
	let mut threads = vec![];
	let mut node = vec![];
	let mut edge = vec![];
	let mut adj: Vec<LinkedList<usize>> = Vec::with_capacity(n);
	let mut excess: Arc<Mutex<VecDeque<usize>>> = Arc::new(Mutex::new(VecDeque::new()));
	let debug = false;

	let s = 0;
	let t = n-1;

	//println!("n = {}", n);
	//println!("m = {}", m);

	for i in 0..n {
		let u:Node = Node::new(i);
		node.push(Arc::new(Mutex::new(u))); 
		adj.push(LinkedList::new());
	}

	for i in 0..m {
		let u: usize = read!();
		let v: usize = read!();
		let c: i32 = read!();
		let e:Edge = Edge::new(u,v,c);
		adj[u].push_back(i);
		adj[v].push_back(i);
		edge.push(Arc::new(Mutex::new(e))); 
	}

	if debug {
		for i in 0..n {
			print!("adj[{}] = ", i);
			let iter = adj[i].iter();

			for e in iter {
				print!("e = {}, ", e);
			}
			println!("");
		}
	}

	//println!("initial pushes");

	// do initial pushes
	node[s].lock().unwrap().h = n as i32;		
	let iter = adj[s].iter();
	let mut source_node = node[s].lock().unwrap();

	for &e in iter {
		let mut e = edge[e].lock().unwrap();
		let mut u = if e.u == s {
			node[e.v].lock().unwrap()
		} else {
			node[e.u].lock().unwrap()
		};

		source_node.e += e.c;

		push(&mut source_node, &mut u, &mut e, &mut excess);
		drop(u);
	}

	drop(source_node);

	for _ in 0 .. num_threads {
        let mut node_clone = node.clone();
        let mut edge_clone = edge.clone();
        let adj_clone = adj.clone();
		let mut excess_clone = Arc::clone(&excess);

		let h = thread::spawn(move || {
			loop {
				// while excess_clone.lock().unwrap().is_empty() {
				// 	println!("Nbr active threads: {}", ACTIVE_THREADS.load(Ordering::SeqCst));
				// 	if ACTIVE_THREADS.load(Ordering::SeqCst) == 0 {
				// 		return;
				// 	}
				// }

				// ACTIVE_THREADS.fetch_add(1, Ordering::SeqCst);
				// {
				// 	let u = excess_clone.lock().unwrap().pop_front().unwrap();
				// 	println!("Start Discharge");
				// 	discharge(u, &mut node_clone, &mut edge_clone, &mut adj_clone, &mut excess_clone);
				// }
				// ACTIVE_THREADS.fetch_sub(1, Ordering::SeqCst);

			
				let tmp = excess_clone.lock().unwrap().pop_front();

				match tmp {
					None => return,
					Some(u) => { discharge(u, &mut node_clone, &mut edge_clone, &adj_clone, &mut excess_clone); }
				}
				
			}
		});
		threads.push(h);
	}

	for h in threads {
		h.join().unwrap();
	}

	println!("f = {}", node[t].lock().unwrap().e);

}
